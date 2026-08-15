#include "DevHttpServer.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <Poseidon/AI/AIUnit.hpp>
#include <Poseidon/AI/EntityAI.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Game/Scripting/Scripts.hpp>
#include <Poseidon/UI/InGame/InGameUI.hpp>
#include <Poseidon/UI/Map/UIMapCommon.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/World/Entities/Vehicles/Misc/Ship.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/World.hpp>
#include <Evaluator/express.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Poseidon::Dev
{
namespace
{
constexpr unsigned short HttpPort = 10001;
constexpr size_t MaxRequestBytes = 64 * 1024;

struct Completion
{
    std::mutex mutex;
    std::condition_variable ready;
    bool done = false;
    int status = 500;
    std::string body;
};

enum class CommandType
{
    Lock,
    ExecuteSqf,
    ExecuteSqs,
};

struct GameCommand
{
    CommandType type;
    uint64_t objectId = 0;
    std::string code;
    std::shared_ptr<Completion> completion;
};

struct HttpResponse
{
    int status;
    const char* contentType;
    std::string body;
};

const char* SideName(TargetSide side)
{
    switch (side)
    {
    case TEast: return "east";
    case TWest: return "west";
    case TGuerrila: return "guerrila";
    case TCivilian: return "civilian";
    case TEnemy: return "enemy";
    case TFriendly: return "friendly";
    case TLogic: return "logic";
    case TEmpty: return "empty";
    default: return "unknown";
    }
}

const char* RelationName(TargetSide side, TargetSide playerSide)
{
    if (side == TCivilian)
        return "civilian";
    if (side == TFriendly || (playerSide != TSideUnknown && side == playerSide))
        return "friendly";
    if (side == TSideUnknown || side == TEmpty || side == TLogic)
        return "unknown";
    return "enemy";
}

const char* MarkerKind(const Entity* entity, const char* fallback)
{
    if (dynamic_cast<const Person*>(entity))
        return "human";
    if (const EntityAI* ai = dynamic_cast<const EntityAI*>(entity); ai && ai->GetType()->GetKind() == VAir)
        return "air";
    if (dynamic_cast<const Ship*>(entity))
        return "water";
    return fallback;
}

// Network IDs are -1 for ordinary local single-player entities.  They are
// therefore not usable as either an API identity or a deduplication key.
// Networked entities retain their ID; all other live entities use their
// address, which remains stable for their lifetime and is safe in JSON's
// integer range on the supported 64-bit builds.
uint64_t EntityKey(const Entity* entity)
{
    if (!entity)
        return 0;
    const int networkId = entity->ID();
    return networkId >= 0 ? static_cast<uint64_t>(networkId)
                          : static_cast<uint64_t>(reinterpret_cast<uintptr_t>(entity));
}

std::string EscapeJson(const char* value)
{
    std::string result;
    if (!value)
        return result;
    for (const unsigned char c : std::string(value))
    {
        switch (c)
        {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char escaped[7];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                result += escaped;
            }
            else
            {
                result += static_cast<char>(c);
            }
        }
    }
    return result;
}

std::string JsonError(const std::string& message)
{
    return "{\"ok\":false,\"error\":\"" + EscapeJson(message.c_str()) + "\"}";
}

std::string JsonOk(const std::string& result)
{
    return "{\"ok\":true,\"result\":\"" + EscapeJson(result.c_str()) + "\"}";
}

std::string UrlDecode(std::string value)
{
    std::string decoded;
    decoded.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == '+' )
        {
            decoded += ' ';
        }
        else if (value[i] == '%' && i + 2 < value.size() && std::isxdigit(static_cast<unsigned char>(value[i + 1])) &&
                 std::isxdigit(static_cast<unsigned char>(value[i + 2])))
        {
            const char hex[] = {value[i + 1], value[i + 2], 0};
            decoded += static_cast<char>(std::strtol(hex, nullptr, 16));
            i += 2;
        }
        else
        {
            decoded += value[i];
        }
    }
    return decoded;
}

std::string QueryValue(const std::string& target, const char* key)
{
    const size_t question = target.find('?');
    if (question == std::string::npos)
        return {};
    const std::string prefix = std::string(key) + "=";
    size_t begin = question + 1;
    while (begin < target.size())
    {
        const size_t end = target.find('&', begin);
        const std::string part = target.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (part.rfind(prefix, 0) == 0)
            return UrlDecode(part.substr(prefix.size()));
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return {};
}

std::string RequestPath(const std::string& target)
{
    const size_t question = target.find('?');
    return target.substr(0, question);
}

const char* DashboardPage()
{
    static constexpr char page[] = R"HTML(<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Poseidon LAN Control</title>
<style>
html,body{width:100%;height:100%;overflow:hidden}*{box-sizing:border-box}body{margin:0;display:grid;grid-template-rows:auto minmax(0,1fr);background:#111820;color:#d9e3ed;font:14px system-ui,sans-serif}header{height:47px;padding:0 18px;background:#192631;display:flex;justify-content:space-between;align-items:center}h1{font-size:18px;margin:0}.warn{color:#ffc76a}main{min-height:0;overflow:hidden;display:grid;grid-template-columns:minmax(0,2fr) minmax(300px,1fr);gap:12px;padding:12px}.card{min-height:0;background:#18232d;border:1px solid #314351;border-radius:7px;padding:10px}.map-card{display:flex;flex-direction:column;gap:8px}.map-frame{position:relative;flex:1;min-height:0;overflow:hidden;background:#071016;border:1px solid #425564;border-radius:4px}.map-frame:after{content:'N';position:absolute;top:8px;left:50%;transform:translateX(-50%);font-weight:700;color:#f3f6f8;text-shadow:0 1px 2px #000;pointer-events:none}canvas{display:block;width:100%;height:100%;cursor:grab;touch-action:none}.legend{margin:0;line-height:18px}.legend span{padding-right:12px}.dot{display:inline-block;width:9px;height:9px;border-radius:99px;margin-right:4px}.friend{background:#4be071}.enemy{background:#ff5964}.civilian{background:#55aaff}.muted{color:#9caebc}aside{min-height:0;overflow:hidden;display:grid;grid-template-rows:auto minmax(145px,.75fr) minmax(0,1fr);gap:12px}.lock-card p{margin:7px 0}textarea{width:100%;height:calc(100% - 67px);min-height:70px;resize:none;background:#081016;color:#d9e3ed;border:1px solid #425564;font:12px ui-monospace,monospace;padding:8px}button,select{background:#295d84;color:#fff;border:0;border-radius:4px;padding:7px 10px;margin:4px 0}button:hover{background:#397eaf}pre{white-space:pre-wrap;overflow:auto;margin:7px 0 0;min-height:26px;max-height:72px;color:#a9d5ff}.units-card{display:flex;flex-direction:column;overflow:hidden}.units-card strong{margin-bottom:5px}#units{min-height:0;overflow:auto}table{border-collapse:collapse;width:100%;font-size:12px}th,td{text-align:left;padding:4px;border-bottom:1px solid #2d3c47}tr:hover{background:#263846;cursor:pointer}@media(max-width:780px){header{height:40px;padding:0 10px}.warn{display:none}main{grid-template-columns:1fr;grid-template-rows:minmax(0,1.1fr) minmax(0,.9fr);gap:8px;padding:8px}aside{gap:8px;grid-template-rows:auto minmax(120px,.8fr) minmax(0,1fr)}}
</style><body><header><h1>Poseidon LAN 控制台</h1><span class="warn">无认证 · 监听 0.0.0.0:10001</span></header><main>
<section class="card map-card"><div class="map-frame"><canvas id="map" width="1200" height="1200"></canvas></div><p class="legend"><span><i class="dot friend"></i>友军</span><span><i class="dot enemy"></i>敌军</span><span><i class="dot civilian"></i>Civilian</span><span class="muted">圆形：人员　方形：地面载具　三角形：空中　梯形：水上　箭头朝向，顶部为北</span><span class="muted" id="summary">等待游戏状态…</span></p></section>
<aside><section class="card lock-card"><strong>地图锁定</strong><p class="muted">滚轮或双指缩放，拖拽平移。点击单位尝试以当前所选武器锁定；仍遵守正常雷达、可见性和武器规则。</p><pre id="lock">尚未请求</pre></section><section class="card"><strong>脚本执行</strong><p><select id="lang"><option value="sqf">SQF（立即执行）</option><option value="sqs">SQS（排队脚本）</option></select><button id="run">运行</button></p><textarea id="code" spellcheck="false">hint "Hello from Poseidon LAN Control";</textarea><pre id="result"></pre></section><section class="card units-card"><strong>存活单位</strong><div id="units"></div></section></aside>
</main><script>
let state={ready:false,units:[],mapSize:1,mapVersion:0};const c=document.querySelector('#map'),ctx=c.getContext('2d'),terrain=new Image();let terrainVersion=-1,pts=[],view={cx:.5,cz:.5,zoom:1},lastSize=0,suppressClick=0;const pointers=new Map();
const color=r=>({friendly:'#4be071',enemy:'#ff5964',civilian:'#55aaff'}[r]||'#d7c75d');const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
function span(){return Math.max(1,state.mapSize||1)/view.zoom}function clamp(){let m=Math.max(1,state.mapSize||1),h=span()/2;view.cx=Math.max(h,Math.min(m-h,view.cx));view.cz=Math.max(h,Math.min(m-h,view.cz))}function screen(x,z){let s=span();return[(x-(view.cx-s/2))/s*c.width,((view.cz+s/2)-z)/s*c.height]}function world(x,y){let s=span();return{x:view.cx-s/2+x/c.width*s,z:view.cz+s/2-y/c.height*s}}function zoomAt(x,y,factor){let p=world(x,y),old=view.zoom;view.zoom=Math.max(1,Math.min(20,view.zoom*factor));if(view.zoom===old)return;let q=world(x,y);view.cx+=p.x-q.x;view.cz+=p.z-q.z;clamp();draw()}
function stepFor(s){let b=10**Math.floor(Math.log10(Math.max(1,s)));return[1,2,5,10].map(v=>v*b).find(v=>v>=s/7)||b*10}function grid(){let s=span(),left=view.cx-s/2,right=view.cx+s/2,bottom=view.cz-s/2,top=view.cz+s/2,step=stepFor(s);ctx.strokeStyle='rgba(18,36,42,.50)';ctx.lineWidth=1;ctx.beginPath();for(let x=Math.ceil(left/step)*step;x<right;x+=step){let a=screen(x,bottom),b=screen(x,top);ctx.moveTo(a[0],a[1]);ctx.lineTo(b[0],b[1])}for(let z=Math.ceil(bottom/step)*step;z<top;z+=step){let a=screen(left,z),b=screen(right,z);ctx.moveTo(a[0],a[1]);ctx.lineTo(b[0],b[1])}ctx.stroke()}
function marker(u,x,y){let r=u.kind==='human'?5:6;ctx.save();ctx.translate(x,y);ctx.fillStyle=color(u.relation);ctx.strokeStyle='#071016';ctx.lineWidth=1.5;ctx.beginPath();if(u.kind==='human')ctx.arc(0,0,r,0,Math.PI*2);else if(u.kind==='air'||u.kind==='fast'){ctx.moveTo(0,-r-1);ctx.lineTo(r,r);ctx.lineTo(-r,r);ctx.closePath()}else if(u.kind==='water'){ctx.moveTo(-r,-r);ctx.lineTo(r,-r);ctx.lineTo(r+2,r);ctx.lineTo(-r-2,r);ctx.closePath()}else ctx.rect(-r,-r,r*2,r*2);ctx.fill();ctx.stroke();let dx=Number(u.dirX)||0,dz=Number(u.dirZ)||0;if(dx||dz){ctx.rotate(Math.atan2(dx,dz));ctx.strokeStyle='#f6fbff';ctx.lineWidth=1.6;ctx.beginPath();ctx.moveTo(0,-r-2);ctx.lineTo(0,-r-10);ctx.moveTo(-3,-r-7);ctx.lineTo(0,-r-10);ctx.lineTo(3,-r-7);ctx.stroke()}ctx.restore()}
function draw(){ctx.clearRect(0,0,c.width,c.height);ctx.fillStyle='#071016';ctx.fillRect(0,0,c.width,c.height);if(terrain.complete&&terrain.naturalWidth){let a=screen(0,state.mapSize),b=screen(state.mapSize,0);ctx.drawImage(terrain,a[0],a[1],b[0]-a[0],b[1]-a[1])}grid();pts=[];for(const u of state.units||[]){if(u.status!=='active')continue;let[x,y]=screen(u.x,u.z);if(x<-14||y<-14||x>c.width+14||y>c.height+14)continue;pts.push([x,y,u]);marker(u,x,y)}document.querySelector('#summary').textContent=`${pts.length}/${(state.units||[]).length} 个存活单位 · 视野 ${Math.round(span())} m · ${Math.round(view.zoom*100)}%`;}
function rows(){let list=(state.units||[]).filter(u=>u.status==='active').slice().sort((a,b)=>a.relation.localeCompare(b.relation)||a.display.localeCompare(b.display));document.querySelector('#units').innerHTML='<table><tr><th>关系</th><th>类型</th><th>位置</th></tr>'+list.map(u=>`<tr data-id="${u.id}"><td style="color:${color(u.relation)}">${esc(u.relation)}</td><td>${esc(u.display||u.type)}</td><td>${Math.round(u.x)}, ${Math.round(u.z)}</td></tr>`).join('')+'</table>';document.querySelectorAll('#units tr[data-id]').forEach(e=>e.onclick=()=>lock(e.dataset.id));}
async function lock(id){let r=await fetch('/api/lock?id='+encodeURIComponent(id),{method:'POST'});document.querySelector('#lock').textContent=JSON.stringify(await r.json(),null,2)}function pick(x,y){let best=null,d=18*18;for(const p of pts){let q=(p[0]-x)**2+(p[1]-y)**2;if(q<d){d=q;best=p[2]}}if(best)lock(best.id)}
function point(e){let r=c.getBoundingClientRect();return{x:(e.clientX-r.left)*c.width/r.width,y:(e.clientY-r.top)*c.height/r.height}}c.addEventListener('wheel',e=>{e.preventDefault();let p=point(e);zoomAt(p.x,p.y,e.deltaY<0?1.18:1/1.18)},{passive:false});c.addEventListener('pointerdown',e=>{let p=point(e);pointers.set(e.pointerId,p);c.setPointerCapture(e.pointerId)});c.addEventListener('pointermove',e=>{if(!pointers.has(e.pointerId))return;let p=point(e),old=pointers.get(e.pointerId);pointers.set(e.pointerId,p);if(pointers.size===1){let s=span();view.cx+=(old.x-p.x)/c.width*s;view.cz+=(p.y-old.y)/c.height*s;clamp();suppressClick=performance.now()+240;draw()}else if(pointers.size===2){let a=[...pointers.values()],d=Math.hypot(a[0].x-a[1].x,a[0].y-a[1].y),mid={x:(a[0].x+a[1].x)/2,y:(a[0].y+a[1].y)/2};if(c._pinch)zoomAt(mid.x,mid.y,d/c._pinch.d);c._pinch={d,mid};suppressClick=performance.now()+240}});function release(e){pointers.delete(e.pointerId);if(pointers.size<2)c._pinch=null}c.addEventListener('pointerup',release);c.addEventListener('pointercancel',release);c.addEventListener('click',e=>{if(performance.now()<suppressClick)return;let p=point(e);pick(p.x,p.y)});
document.querySelector('#run').onclick=async()=>{let lang=document.querySelector('#lang').value,code=document.querySelector('#code').value,r=await fetch('/api/exec?lang='+lang,{method:'POST',body:code}),text=await r.text();document.querySelector('#result').textContent=text};terrain.onload=draw;
async function refresh(){try{let r=await fetch('/api/state',{cache:'no-store'});state=await r.json();if(state.mapSize!==lastSize){lastSize=state.mapSize;view={cx:state.mapSize/2,cz:state.mapSize/2,zoom:1}}if(state.mapVersion!==terrainVersion){terrainVersion=state.mapVersion;terrain.src='/api/map?v='+terrainVersion}draw();rows()}catch(e){document.querySelector('#summary').textContent='连接失败: '+e}}refresh();setInterval(refresh,500);
</script></body></html>)HTML";
    // Keep the dashboard self-contained, but layer the native-map affordances
    // here so the readable HTML above remains compact.  GridInfo and
    // PositionToAA11 come from the same CfgWorlds data and formatter that the
    // in-game map uses.
    static const std::string enhanced = []
    {
        std::string html(page);
        static constexpr char additions[] = R"JS(
<style>
.follow-toggle{display:inline-flex;align-items:center;gap:5px;margin-right:12px;cursor:pointer;color:#d9e3ed;user-select:none}.follow-toggle input{accent-color:#4be071}
</style>
<script>
document.querySelector('.legend').insertAdjacentHTML('afterbegin','<label class="follow-toggle" title="开启后，地图会在每次状态刷新时重新以玩家为中心"><input id="follow-player" type="checkbox">跟踪玩家</label>');
const followPlayerControl=document.querySelector('#follow-player');let followPlayer=false;
function playerUnit(){return(state.units||[]).find(u=>u.isPlayer&&u.status==='active')}
function centerOnPlayer(){const u=playerUnit();if(!u)return;view.cx=u.x;view.cz=u.z}
followPlayerControl.addEventListener('change',()=>{followPlayer=followPlayerControl.checked;if(followPlayer){centerOnPlayer();draw()}});
const drawBeforeFollow=draw;draw=()=>{if(followPlayer)centerOnPlayer();drawBeforeFollow()};
const markerBeforePlayer=marker;marker=(u,x,y)=>{markerBeforePlayer(u,x,y);if(u.isPlayer){ctx.save();ctx.strokeStyle='#ffffff';ctx.lineWidth=1.5;ctx.beginPath();ctx.arc(x,y,10,0,Math.PI*2);ctx.stroke();ctx.restore()}};
function gridPart(format,value){
  const first=[...format].findIndex(c=>/[0-9A-Za-z]/.test(c));if(first<0)return'';let i=Math.trunc(value),rev='';
  const take=base=>{const q=Math.trunc(i/base),m=i-q*base;i=q;return m};
  for(let p=format.length-1;p>first;--p){const ch=format[p];if(/[0-9]/.test(ch))rev+=String.fromCharCode(48+((take(10)+(ch.charCodeAt(0)-48))%10+10)%10);else if(/[A-J]/.test(ch))rev+=String.fromCharCode(65+((take(10)+(ch.charCodeAt(0)-65))%10+10)%10);else if(/[a-j]/.test(ch))rev+=String.fromCharCode(97+((take(10)+(ch.charCodeAt(0)-97))%10+10)%10);else rev+=ch}
  const ch=format[first];if(/[0-9]/.test(ch))rev+=String.fromCharCode(48+((take(10)+(ch.charCodeAt(0)-48))%10+10)%10);else if(/[A-Z]/.test(ch))rev+=String.fromCharCode(65+((take(26)+(ch.charCodeAt(0)-65))%26+26)%26);else if(/[a-z]/.test(ch))rev+=String.fromCharCode(97+((take(26)+(ch.charCodeAt(0)-97))%26+26)%26);else rev+=ch;
  for(let p=first-1;p>=0;--p)rev+=format[p];return[...rev].reverse().join('')
}
function gameGrid(){const levels=state.grid||[],scale=(state.mapSize?span()/state.mapSize:1);return levels.find(g=>scale<=g.zoomMax)||levels[levels.length-1]}
const genericGrid=grid;grid=()=>{
  const g=gameGrid();if(!g||!g.stepX||!g.stepY){genericGrid();return}const s=span(),left=view.cx-s/2,right=view.cx+s/2,bottom=view.cz-s/2,top=view.cz+s/2,size=state.mapSize||1;
  const startX=Math.ceil((left-g.offsetX)/g.stepX),endX=Math.floor((right-g.offsetX)/g.stepX),startY=Math.ceil((size-top-g.offsetY)/g.stepY),endY=Math.floor((size-bottom-g.offsetY)/g.stepY);
  ctx.save();ctx.strokeStyle='rgba(220,230,216,.38)';ctx.fillStyle='rgba(239,246,237,.90)';ctx.lineWidth=1;ctx.font='600 13px system-ui,sans-serif';ctx.textAlign='center';ctx.textBaseline='top';ctx.beginPath();
  for(let i=startX;i<=endX;i++){const x=i*g.stepX+g.offsetX,a=screen(x,bottom),b=screen(x,top);ctx.moveTo(a[0],a[1]);ctx.lineTo(b[0],b[1])}for(let i=startY;i<=endY;i++){const z=size-(i*g.stepY+g.offsetY),a=screen(left,z),b=screen(right,z);ctx.moveTo(a[0],a[1]);ctx.lineTo(b[0],b[1])}ctx.stroke();
  for(let i=startX;i<=endX;i++){const x=i*g.stepX+g.offsetX,p=screen(x,top),label=gridPart(g.formatX,g.stepX>=0?i:i-1);ctx.fillText(label,p[0],p[1]+3);ctx.textBaseline='bottom';ctx.fillText(label,p[0],c.height-3);ctx.textBaseline='top'}
  ctx.textAlign='left';ctx.textBaseline='middle';for(let i=startY;i<=endY;i++){const z=size-(i*g.stepY+g.offsetY),p=screen(left,z),label=gridPart(g.formatY,g.stepY>=0?i:i-1);ctx.fillText(label,4,p[1]);ctx.textAlign='right';ctx.fillText(label,c.width-4,p[1]);ctx.textAlign='left'}ctx.restore()
};
const rowsBeforeGridReference=rows;rows=()=>{rowsBeforeGridReference();document.querySelectorAll('#units tr[data-id]').forEach(row=>{const unit=(state.units||[]).find(u=>String(u.id)===row.dataset.id),cell=row.querySelector('td:last-child');if(unit&&cell&&unit.gridRef)cell.textContent=`${unit.gridRef} (${Math.round(unit.x)}, ${Math.round(unit.z)})`})};
</script>
)JS";
        const size_t scriptEnd = html.rfind("</script>");
        html.insert(scriptEnd == std::string::npos ? html.size() : scriptEnd + std::strlen("</script>"), additions);
        return html;
    }();
    return enhanced.c_str();
}

class Server
{
public:
    void Start()
    {
        if (_thread.joinable())
            return;
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            LOG_ERROR(Core, "HTTP server: WSAStartup failed");
            return;
        }
        _stopping = false;
        _thread = std::thread(&Server::Run, this);
    }

    void Stop()
    {
        _stopping = true;
        if (_thread.joinable())
            _thread.join();
        WSACleanup();
    }

    void Pump()
    {
        PublishTerrainMap();
        PublishSnapshot();

        std::deque<GameCommand> commands;
        {
            std::lock_guard lock(_mutex);
            commands.swap(_commands);
        }
        for (GameCommand& command : commands)
        {
            switch (command.type)
            {
            case CommandType::Lock:
            {
                const bool locked = TryLock(command.objectId);
                Complete(command.completion, locked ? 200 : 409,
                    locked ? JsonOk("target locked") : JsonError("target is not currently lockable"));
                break;
            }
            case CommandType::ExecuteSqf:
                Complete(command.completion, 200, ExecuteSqf(command.code));
                break;
            case CommandType::ExecuteSqs:
                Complete(command.completion, 200, ExecuteSqs(command.code));
                break;
            }
        }
    }

private:
    std::atomic<bool> _stopping{false};
    SOCKET _listenSocket = INVALID_SOCKET;
    std::thread _thread;
    std::mutex _mutex;
    std::deque<GameCommand> _commands;
    std::string _snapshot = "{\"ready\":false,\"units\":[]}";
    std::string _terrainMap;
    uint64_t _terrainMapSignature = 0;
    int _terrainMapVersion = 0;

    static void WriteLE16(std::string& bytes, size_t offset, uint16_t value)
    {
        bytes[offset] = static_cast<char>(value & 0xff);
        bytes[offset + 1] = static_cast<char>((value >> 8) & 0xff);
    }

    static void WriteLE32(std::string& bytes, size_t offset, uint32_t value)
    {
        for (size_t i = 0; i < 4; ++i)
            bytes[offset + i] = static_cast<char>((value >> (i * 8)) & 0xff);
    }

    void PublishTerrainMap()
    {
        if (!GLandscape)
            return;

        const int landRange = GLandscape->GetLandRange();
        const int terrainRange = GLandscape->GetTerrainRange();
        if (landRange <= 1 || terrainRange <= 1)
            return;
        const uint64_t signature = (uint64_t(landRange) << 40) ^ (uint64_t(terrainRange) << 24) ^
                                   std::hash<std::string>{}(std::string((const char*)Glob.header.worldname));
        {
            std::lock_guard lock(_mutex);
            if (_terrainMapSignature == signature && !_terrainMap.empty())
                return;
        }

        // This height-and-relief image comes directly from the loaded game
        // landscape, so its X/Z coordinates exactly match the live unit map.
        constexpr int imageSize = 512;
        std::vector<float> heights(imageSize * imageSize);
        const int terrainShift = std::max(0, GLandscape->GetTerrainRangeLog() - GLandscape->GetLandRangeLog());
        float minHeight = std::numeric_limits<float>::max();
        float maxHeight = std::numeric_limits<float>::lowest();
        for (int y = 0; y < imageSize; ++y)
        {
            const int landZ = ((imageSize - 1 - y) * (landRange - 1)) / (imageSize - 1); // top = north / +Z
            const int terrainZ = std::min(terrainRange - 1, landZ << terrainShift);
            for (int x = 0; x < imageSize; ++x)
            {
                const int landX = (x * (landRange - 1)) / (imageSize - 1);
                const int terrainX = std::min(terrainRange - 1, landX << terrainShift);
                const float height = GLandscape->GetHeight(terrainZ, terrainX);
                heights[y * imageSize + x] = height;
                minHeight = std::min(minHeight, height);
                maxHeight = std::max(maxHeight, height);
            }
        }

        const float terrainSpan = std::max(1.0f, maxHeight - std::max(0.0f, minHeight));
        const size_t rowStride = (imageSize * 3 + 3) & ~size_t(3);
        std::string bitmap(54 + rowStride * imageSize, '\0');
        bitmap[0] = 'B';
        bitmap[1] = 'M';
        WriteLE32(bitmap, 2, static_cast<uint32_t>(bitmap.size()));
        WriteLE32(bitmap, 10, 54);
        WriteLE32(bitmap, 14, 40);
        WriteLE32(bitmap, 18, imageSize);
        WriteLE32(bitmap, 22, imageSize);
        WriteLE16(bitmap, 26, 1);
        WriteLE16(bitmap, 28, 24);
        WriteLE32(bitmap, 34, static_cast<uint32_t>(rowStride * imageSize));
        for (int bitmapY = 0; bitmapY < imageSize; ++bitmapY)
        {
            const int sourceY = imageSize - 1 - bitmapY; // BMP storage is bottom-up
            for (int x = 0; x < imageSize; ++x)
            {
                const float height = heights[sourceY * imageSize + x];
                const float east = heights[sourceY * imageSize + std::min(x + 1, imageSize - 1)];
                const float south = heights[std::min(sourceY + 1, imageSize - 1) * imageSize + x];
                const float relief = std::min(1.0f, (std::abs(east - height) + std::abs(south - height)) * 0.025f);
                uint8_t red, green, blue;
                if (height < 0.0f)
                {
                    const float depth = std::min(1.0f, -height / 35.0f);
                    red = static_cast<uint8_t>(42 - 15 * depth);
                    green = static_cast<uint8_t>(108 - 26 * depth);
                    blue = static_cast<uint8_t>(142 - 20 * depth);
                }
                else
                {
                    const float level = std::clamp(height / terrainSpan, 0.0f, 1.0f);
                    red = static_cast<uint8_t>(72 + 82 * level - 34 * relief);
                    green = static_cast<uint8_t>(103 + 88 * level - 28 * relief);
                    blue = static_cast<uint8_t>(58 + 53 * level - 20 * relief);
                }
                const size_t pixel = 54 + bitmapY * rowStride + x * 3;
                bitmap[pixel] = static_cast<char>(blue);
                bitmap[pixel + 1] = static_cast<char>(green);
                bitmap[pixel + 2] = static_cast<char>(red);
            }
        }
        std::lock_guard lock(_mutex);
        _terrainMap = std::move(bitmap);
        _terrainMapSignature = signature;
        ++_terrainMapVersion;
    }

    void PublishSnapshot()
    {
        if (!GWorld)
            return;

        std::unordered_set<uint64_t> seen;
        std::ostringstream json;
        const float mapSize = GLandscape ? GLandscape->GetLandRange() * GLandscape->GetLandGrid() : 0.0f;
        TargetSide playerSide = TSideUnknown;
        EntityAI* playerVehicle = nullptr;
        if (AIUnit* player = GWorld->FocusOn())
        {
            playerVehicle = player->GetVehicle();
            if (playerVehicle)
                playerSide = playerVehicle->GetTargetSide();
        }
        int mapVersion = 0;
        {
            std::lock_guard lock(_mutex);
            mapVersion = _terrainMapVersion;
        }
        json << "{\"ready\":true,\"mapSize\":" << mapSize << ",\"mapVersion\":" << mapVersion
             << ",\"mode\":" << int(GWorld->GetMode())
             << ",\"gridOffsetX\":" << GWorld->GetGridOffsetX() << ",\"gridOffsetY\":" << GWorld->GetGridOffsetY()
             << ",\"grid\":[";
        const GridInfo* previousGrid = nullptr;
        bool firstGrid = true;
        // The browser covers map scales 1.0 down to 0.05.  Sampling produces
        // each native CfgWorlds grid level without exposing World internals.
        for (int sample = 0; sample <= 1000; ++sample)
        {
            const GridInfo* info = GWorld->GetGridInfo(float(sample) / 1000.0f);
            if (!info || info == previousGrid)
                continue;
            previousGrid = info;
            if (!firstGrid)
                json << ',';
            firstGrid = false;
            json << "{\"zoomMax\":" << info->zoomMax << ",\"format\":\"" << EscapeJson((const char*)info->format)
                 << "\",\"formatX\":\"" << EscapeJson((const char*)info->formatX) << "\",\"formatY\":\""
                 << EscapeJson((const char*)info->formatY) << "\",\"stepX\":" << info->stepX << ",\"stepY\":" << info->stepY
                 << ",\"offsetX\":" << GWorld->GetGridOffsetX() << ",\"offsetY\":" << GWorld->GetGridOffsetY() << '}';
        }
        json << "],\"units\":[";
        bool first = true;
        auto append = [&](Entity* entity, const char* kind)
        {
            if (!entity || entity->ToDelete() || entity->IsDammageDestroyed())
                return;
            const EntityAI* ai = dynamic_cast<const EntityAI*>(entity);
            // World lists also contain cameras, tracks and other rendering
            // helpers.  They are not game units and must not become map icons.
            if (!ai)
                return;
            const uint64_t entityKey = EntityKey(entity);
            if (entityKey == 0 || !seen.insert(entityKey).second)
                return;
            Vector3Val pos = entity->Position();
            Vector3Val speed = entity->Speed();
            Vector3Val direction = entity->Direction();
            const RString display = entity->GetDisplayName();
            const TargetSide side = ai->GetTargetSide();
            char gridRef[64]{};
            PositionToAA11(pos, gridRef);
            if (!first)
                json << ',';
            first = false;
            json << "{\"id\":" << entityKey << ",\"kind\":\"" << MarkerKind(entity, kind) << "\",\"type\":\""
                 << EscapeJson(entity->GetName()) << "\",\"display\":\"" << EscapeJson((const char*)display) << "\",\"side\":\""
                 << SideName(side) << "\",\"relation\":\"" << RelationName(side, playerSide)
                 << "\",\"status\":\"active\",\"isPlayer\":" << (entity == playerVehicle ? "true" : "false")
                 << ",\"gridRef\":\"" << EscapeJson(gridRef) << "\",\"damage\":"
                 << entity->GetTotalDammage() << ",\"x\":" << pos.X() << ",\"y\":" << pos.Y() << ",\"z\":"
                 << pos.Z() << ",\"speed\":" << speed.Size() << ",\"dirX\":" << direction.X() << ",\"dirZ\":"
                 << direction.Z() << '}';
        };
        for (int i = 0; i < GWorld->NVehicles(); ++i) append(GWorld->GetVehicle(i), "vehicle");
        for (int i = 0; i < GWorld->NAnimals(); ++i) append(GWorld->GetAnimal(i), "animal");
        for (int i = 0; i < GWorld->NOutVehicles(); ++i) append(GWorld->GetOutVehicle(i), "person");
        for (int i = 0; i < GWorld->NFastVehicles(); ++i) append(GWorld->GetFastVehicle(i), "fast");
        json << "]}";
        std::lock_guard lock(_mutex);
        _snapshot = json.str();
    }

    EntityAI* FindAI(uint64_t objectId) const
    {
        if (!GWorld || objectId == 0)
            return nullptr;
        auto find = [objectId](Entity* entity) -> EntityAI*
        {
            if (!entity || EntityKey(entity) != objectId)
                return nullptr;
            return dynamic_cast<EntityAI*>(entity);
        };
        for (int i = 0; i < GWorld->NVehicles(); ++i) if (EntityAI* ai = find(GWorld->GetVehicle(i))) return ai;
        for (int i = 0; i < GWorld->NAnimals(); ++i) if (EntityAI* ai = find(GWorld->GetAnimal(i))) return ai;
        for (int i = 0; i < GWorld->NOutVehicles(); ++i) if (EntityAI* ai = find(GWorld->GetOutVehicle(i))) return ai;
        return nullptr;
    }

    bool TryLock(uint64_t objectId) const
    {
        EntityAI* target = FindAI(objectId);
        AbstractUI* ui = GWorld ? GWorld->UI() : nullptr;
        return ui && ui->LockTargetFromExternal(target);
    }

    std::string ExecuteSqf(const std::string& code) const
    {
        if (!GWorld || code.empty())
            return JsonError("SQF code or game state is unavailable");
        static GameVarSpace scope;
        GameState* state = GWorld->GetGameState();
        state->BeginContext(&scope);
        const GameValue result = state->EvaluateMultiple(code.c_str());
        const EvalError error = state->GetLastError();
        const RString errorText = state->GetLastErrorText();
        const RString resultText = result.GetText();
        state->EndContext();
        if (error != EvalOK)
            return JsonError((const char*)errorText);
        return JsonOk((const char*)resultText);
    }

    std::string ExecuteSqs(const std::string& code) const
    {
        if (!GWorld || code.empty())
            return JsonError("SQS code or game state is unavailable");
        AutoArray<RString> lines;
        size_t begin = 0;
        while (begin <= code.size())
        {
            const size_t end = code.find('\n', begin);
            std::string line = code.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.Add(RString(line.c_str()));
            if (end == std::string::npos)
                break;
            begin = end + 1;
        }
        Script* script = new Script(lines, GameValue());
        script->SetName("http.sqs");
        GWorld->AddScript(script);
        return JsonOk("scheduled");
    }

    static void Complete(const std::shared_ptr<Completion>& completion, int status, std::string body)
    {
        std::lock_guard lock(completion->mutex);
        completion->status = status;
        completion->body = std::move(body);
        completion->done = true;
        completion->ready.notify_one();
    }

    HttpResponse AskGame(CommandType type, uint64_t objectId, std::string code)
    {
        auto completion = std::make_shared<Completion>();
        {
            std::lock_guard lock(_mutex);
            _commands.push_back({type, objectId, std::move(code), completion});
        }
        std::unique_lock lock(completion->mutex);
        completion->ready.wait_for(lock, std::chrono::seconds(2), [&] { return completion->done || _stopping.load(); });
        if (!completion->done)
            return {503, "application/json; charset=utf-8", JsonError("game thread did not answer")};
        return {completion->status, "application/json; charset=utf-8", completion->body};
    }

    static bool SendAll(SOCKET socket, const char* data, size_t bytes)
    {
        while (bytes)
        {
            const int sent = send(socket, data, static_cast<int>(std::min<size_t>(bytes, INT_MAX)), 0);
            if (sent <= 0)
                return false;
            data += sent;
            bytes -= static_cast<size_t>(sent);
        }
        return true;
    }

    static const char* StatusText(int status)
    {
        switch (status)
        {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 503: return "Service Unavailable";
        default: return "Internal Server Error";
        }
    }

    static void SendResponse(SOCKET socket, const HttpResponse& response)
    {
        std::ostringstream headers;
        headers << "HTTP/1.1 " << response.status << ' ' << StatusText(response.status) << "\r\n"
                << "Content-Type: " << response.contentType << "\r\n"
                << "Content-Length: " << response.body.size() << "\r\n"
                << "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
        const std::string headerText = headers.str();
        SendAll(socket, headerText.data(), headerText.size());
        SendAll(socket, response.body.data(), response.body.size());
    }

    void HandleClient(SOCKET socket)
    {
        DWORD timeout = 250;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::string request;
        request.reserve(4096);
        char buffer[4096];
        size_t headerEnd = std::string::npos;
        size_t contentLength = 0;
        while (request.size() < MaxRequestBytes)
        {
            const int received = recv(socket, buffer, sizeof(buffer), 0);
            if (received <= 0)
                return;
            request.append(buffer, received);
            if (headerEnd == std::string::npos)
            {
                headerEnd = request.find("\r\n\r\n");
                if (headerEnd != std::string::npos)
                {
                    const std::string headers = request.substr(0, headerEnd);
                    const std::string marker = "Content-Length:";
                    const size_t at = headers.find(marker);
                    if (at != std::string::npos)
                        contentLength = static_cast<size_t>(std::strtoul(headers.c_str() + at + marker.size(), nullptr, 10));
                    if (headerEnd + 4 + contentLength > MaxRequestBytes)
                    {
                        SendResponse(socket, {413, "application/json; charset=utf-8", JsonError("request exceeds 64 KiB")});
                        return;
                    }
                }
            }
            if (headerEnd != std::string::npos && request.size() >= headerEnd + 4 + contentLength)
                break;
        }
        if (headerEnd == std::string::npos)
        {
            SendResponse(socket, {400, "application/json; charset=utf-8", JsonError("invalid HTTP request")});
            return;
        }

        const std::string firstLine = request.substr(0, request.find("\r\n"));
        const size_t firstSpace = firstLine.find(' ');
        const size_t secondSpace = firstLine.find(' ', firstSpace == std::string::npos ? 0 : firstSpace + 1);
        if (firstSpace == std::string::npos || secondSpace == std::string::npos)
        {
            SendResponse(socket, {400, "application/json; charset=utf-8", JsonError("invalid request line")});
            return;
        }
        const std::string method = firstLine.substr(0, firstSpace);
        const std::string target = firstLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        const std::string path = RequestPath(target);
        const std::string body = request.substr(headerEnd + 4, contentLength);

        if (method == "OPTIONS")
        {
            SendResponse(socket, {200, "text/plain; charset=utf-8", {}});
            return;
        }
        if (method == "GET" && path == "/")
        {
            SendResponse(socket, {200, "text/html; charset=utf-8", DashboardPage()});
            return;
        }
        if (method == "GET" && path == "/api/state")
        {
            std::string snapshot;
            {
                std::lock_guard lock(_mutex);
                snapshot = _snapshot;
            }
            SendResponse(socket, {200, "application/json; charset=utf-8", std::move(snapshot)});
            return;
        }
        if (method == "GET" && path == "/api/map")
        {
            std::string terrainMap;
            {
                std::lock_guard lock(_mutex);
                terrainMap = _terrainMap;
            }
            if (terrainMap.empty())
                SendResponse(socket, {503, "application/json; charset=utf-8", JsonError("terrain map is not ready")});
            else
                SendResponse(socket, {200, "image/bmp", std::move(terrainMap)});
            return;
        }
        if (method == "POST" && path == "/api/lock")
        {
            const std::string idText = QueryValue(target, "id");
            char* end = nullptr;
            const uint64_t id = std::strtoull(idText.c_str(), &end, 10);
            if (idText.empty() || !end || *end != 0 || id == 0)
                SendResponse(socket, {400, "application/json; charset=utf-8", JsonError("id must be an object id")});
            else
                SendResponse(socket, AskGame(CommandType::Lock, id, {}));
            return;
        }
        if (method == "POST" && path == "/api/exec")
        {
            const std::string language = QueryValue(target, "lang");
            if (language == "sqf")
                SendResponse(socket, AskGame(CommandType::ExecuteSqf, 0, body));
            else if (language == "sqs")
                SendResponse(socket, AskGame(CommandType::ExecuteSqs, 0, body));
            else
                SendResponse(socket, {400, "application/json; charset=utf-8", JsonError("lang must be sqf or sqs")});
            return;
        }
        SendResponse(socket, {method == "GET" || method == "POST" ? 404 : 405, "application/json; charset=utf-8",
                              JsonError("endpoint not found")});
    }

    void Run()
    {
        _listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (_listenSocket == INVALID_SOCKET)
        {
            LOG_ERROR(Core, "HTTP server: could not create socket ({})", WSAGetLastError());
            return;
        }
        BOOL reuse = TRUE;
        setsockopt(_listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(HttpPort);
        if (bind(_listenSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
            listen(_listenSocket, SOMAXCONN) == SOCKET_ERROR)
        {
            LOG_ERROR(Core, "HTTP server: failed to listen on 0.0.0.0:{} ({})", HttpPort, WSAGetLastError());
            closesocket(_listenSocket);
            _listenSocket = INVALID_SOCKET;
            return;
        }
        LOG_INFO(Core, "HTTP server: LAN dashboard available at http://0.0.0.0:{}/", HttpPort);
        while (!_stopping)
        {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(_listenSocket, &readable);
            timeval timeout{0, 250000};
            const int selected = select(0, &readable, nullptr, nullptr, &timeout);
            if (selected <= 0 || _stopping)
                continue;
            SOCKET client = accept(_listenSocket, nullptr, nullptr);
            if (client == INVALID_SOCKET)
                continue;
            HandleClient(client);
            closesocket(client);
        }
        if (_listenSocket != INVALID_SOCKET)
        {
            closesocket(_listenSocket);
            _listenSocket = INVALID_SOCKET;
        }
    }
};

std::unique_ptr<Server> gServer;

} // namespace

void StartHttpServer()
{
    if (!gServer)
        gServer = std::make_unique<Server>();
    gServer->Start();
}

void PumpHttpServer()
{
    if (gServer)
        gServer->Pump();
}

void StopHttpServer()
{
    if (!gServer)
        return;
    gServer->Stop();
    gServer.reset();
}

} // namespace Poseidon::Dev

#else

namespace Poseidon::Dev
{
void StartHttpServer() {}
void PumpHttpServer() {}
void StopHttpServer() {}
} // namespace Poseidon::Dev

#endif
