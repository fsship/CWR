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
#include <Poseidon/IO/Filesystem/Utf8Paths.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/UI/InGame/InGameUI.hpp>
#include <Poseidon/UI/Map/UIMapCommon.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/World/Entities/Vehicles/Misc/Ship.hpp>
#include <Poseidon/World/MapTypes.hpp>
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

std::string WebAssetPath(const char* fileName)
{
    const char* configuredDirectory = std::getenv("POSEIDON_WEB_DIR");
    if (configuredDirectory && *configuredDirectory)
        return std::string(configuredDirectory) + "\\" + fileName;

    char executablePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(nullptr, executablePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    std::string directory(executablePath, length);
    const size_t separator = directory.find_last_of("\\/");
    if (separator == std::string::npos)
        return {};
    directory.resize(separator + 1);
    return directory + "web\\" + fileName;
}

std::string ReadWebAsset(const char* fileName)
{
    const std::string path = WebAssetPath(fileName);
    if (path.empty())
        return {};
    const std::vector<char> bytes = ReadFileUtf8(path.c_str());
    return bytes.empty() ? std::string() : std::string(bytes.begin(), bytes.end());
}

const char* DashboardPage()
{
    static constexpr char page[] = R"HTML(<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Poseidon LAN Control</title>
<style>
html,body{width:100%;height:100%;overflow:hidden}*{box-sizing:border-box}body{margin:0;display:grid;grid-template-rows:auto minmax(0,1fr);background:#111820;color:#d9e3ed;font:14px system-ui,sans-serif}header{height:47px;padding:0 18px;background:#192631;display:flex;justify-content:space-between;align-items:center}h1{font-size:18px;margin:0}.warn{color:#ffc76a}main{min-height:0;overflow:hidden;display:grid;grid-template-columns:minmax(0,2fr) minmax(300px,1fr);gap:12px;padding:12px}.card{min-height:0;background:#18232d;border:1px solid #314351;border-radius:7px;padding:10px}.map-card{display:flex;flex-direction:column;gap:8px}.map-frame{position:relative;flex:1;min-height:0;overflow:hidden;background:#c8e6fd;border:1px solid #425564;border-radius:4px}.map-frame:after{content:'N';position:absolute;top:8px;left:50%;transform:translateX(-50%);font-weight:700;color:#f3f6f8;text-shadow:0 1px 2px #000;pointer-events:none}canvas{display:block;width:100%;height:100%;cursor:grab;touch-action:none}.legend{margin:0;line-height:18px}.legend span{padding-right:12px}.dot{display:inline-block;width:9px;height:9px;border-radius:99px;margin-right:4px}.friend{background:#4be071}.enemy{background:#ff5964}.civilian{background:#55aaff}.muted{color:#9caebc}aside{min-height:0;overflow:hidden;display:grid;grid-template-rows:auto minmax(145px,.75fr) minmax(0,1fr);gap:12px}.lock-card p{margin:7px 0}textarea{width:100%;height:calc(100% - 67px);min-height:70px;resize:none;background:#081016;color:#d9e3ed;border:1px solid #425564;font:12px ui-monospace,monospace;padding:8px}button,select{background:#295d84;color:#fff;border:0;border-radius:4px;padding:7px 10px;margin:4px 0}button:hover{background:#397eaf}pre{white-space:pre-wrap;overflow:auto;margin:7px 0 0;min-height:26px;max-height:72px;color:#a9d5ff}.units-card{display:flex;flex-direction:column;overflow:hidden}.units-card strong{margin-bottom:5px}#units{min-height:0;overflow:auto}table{border-collapse:collapse;width:100%;font-size:12px}th,td{text-align:left;padding:4px;border-bottom:1px solid #2d3c47}#units th{position:sticky;top:0;z-index:2;background:#18232d;box-shadow:0 1px 0 #425564}tr:hover{background:#263846;cursor:pointer}tr.locked-target td{background:#6b4200!important;color:#fff5d2;font-weight:700;border-bottom-color:#e7a226}tr.locked-target:hover td{background:#835200!important}@media(max-width:780px){header{height:40px;padding:0 10px}.warn{display:none}main{grid-template-columns:1fr;grid-template-rows:minmax(0,1.1fr) minmax(0,.9fr);gap:8px;padding:8px}aside{gap:8px;grid-template-rows:auto minmax(120px,.8fr) minmax(0,1fr)}}
</style><body><header><h1>Poseidon LAN 控制台</h1><span class="warn">无认证 · 监听 0.0.0.0:10001</span></header><main>
<section class="card map-card"><div class="map-frame"><canvas id="map" width="1200" height="1200"></canvas></div><p class="legend"><span><i class="dot friend"></i>友军</span><span><i class="dot enemy"></i>敌军</span><span><i class="dot civilian"></i>Civilian</span><span class="muted">圆形：人员　方形：地面载具　三角形：空中　梯形：水上　箭头朝向，顶部为北</span><span class="muted" id="summary">等待游戏状态…</span></p></section>
<aside><section class="card lock-card"><strong>地图锁定</strong><p class="muted">滚轮或双指缩放，拖拽平移。点击单位尝试以当前所选武器锁定；仍遵守正常雷达、可见性和武器规则。</p><pre id="lock">尚未请求</pre></section><section class="card"><strong>脚本执行</strong><p><select id="lang"><option value="sqf">SQF（立即执行）</option><option value="sqs">SQS（排队脚本）</option></select><button id="run">运行</button></p><textarea id="code" spellcheck="false">hint "Hello from Poseidon LAN Control";</textarea><pre id="result"></pre></section><section class="card units-card"><strong>存活单位</strong><div id="units"></div></section></aside>
</main><script>
let state={ready:false,units:[],locations:[],mapSize:1,mapVersion:0,lockedId:0};const c=document.querySelector('#map'),ctx=c.getContext('2d'),terrain=new Image(),mapCache=document.createElement('canvas'),mapCacheCtx=mapCache.getContext('2d');ctx.imageSmoothingEnabled=true;ctx.imageSmoothingQuality='high';let terrainVersion=-1,vectorMap=null,darkMap=false,mapCacheInfo=null,dragSettleTimer=0,zoomPreview=null,zoomSettleTimer=0,pts=[],view={cx:.5,cz:.5,zoom:1},lastSize=0,suppressClick=0,dragStart=null,wasDragging=false;const pointers=new Map();
const color=r=>({friendly:'#4be071',enemy:'#ff5964',civilian:'#55aaff'}[r]||'#d7c75d');const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
function span(){return Math.max(1,state.mapSize||1)/view.zoom}function spans(){const s=span(),rect=c.getBoundingClientRect(),aspect=rect.width/Math.max(1,rect.height);return aspect>=1?{x:s,z:s/aspect}:{x:s*aspect,z:s}}function clamp(){let m=Math.max(1,state.mapSize||1),v=spans(),hx=v.x/2,hz=v.z/2;view.cx=v.x>=m?m/2:Math.max(hx,Math.min(m-hx,view.cx));view.cz=v.z>=m?m/2:Math.max(hz,Math.min(m-hz,view.cz))}function screen(x,z){let v=spans();return[(x-(view.cx-v.x/2))/v.x*c.width,((view.cz+v.z/2)-z)/v.z*c.height]}function world(x,y){let v=spans();return{x:view.cx-v.x/2+x/c.width*v.x,z:view.cz+v.z/2-y/c.height*v.z}}function zoomAt(x,y,factor){let p=world(x,y),old=view.zoom;view.zoom=Math.max(1,Math.min(20,view.zoom*factor));if(view.zoom===old)return;let q=world(x,y);view.cx+=p.x-q.x;view.cz+=p.z-q.z;clamp();zoomPreview={x,y};clearTimeout(zoomSettleTimer);zoomSettleTimer=setTimeout(()=>{zoomPreview=null;draw(false)},90);draw(true)}
function stepFor(s){let b=10**Math.floor(Math.log10(Math.max(1,s)));return[1,2,5,10].map(v=>v*b).find(v=>v>=s/7)||b*10}function grid(){let v=spans(),left=view.cx-v.x/2,right=view.cx+v.x/2,bottom=view.cz-v.z/2,top=view.cz+v.z/2,step=stepFor(Math.max(v.x,v.z));ctx.strokeStyle=darkMap?'rgba(181,197,180,.42)':'rgba(65,67,50,.58)';ctx.lineWidth=1;ctx.beginPath();for(let x=Math.ceil(left/step)*step;x<right;x+=step){let a=screen(x,bottom),b=screen(x,top);ctx.moveTo(a[0],a[1]);ctx.lineTo(b[0],b[1])}for(let z=Math.ceil(bottom/step)*step;z<top;z+=step){let a=screen(left,z),b=screen(right,z);ctx.moveTo(a[0],a[1]);ctx.lineTo(b[0],b[1])}ctx.stroke()}
function marker(u,x,y){let r=u.kind==='human'?9:11;ctx.save();ctx.translate(x,y);ctx.fillStyle=color(u.relation);ctx.strokeStyle='#071016';ctx.lineWidth=2;ctx.beginPath();if(u.kind==='human')ctx.arc(0,0,r,0,Math.PI*2);else if(u.kind==='air'||u.kind==='fast'){ctx.moveTo(0,-r-2);ctx.lineTo(r+1,r);ctx.lineTo(-r-1,r);ctx.closePath()}else if(u.kind==='water'){ctx.moveTo(-r,-r);ctx.lineTo(r,-r);ctx.lineTo(r+3,r);ctx.lineTo(-r-3,r);ctx.closePath()}else ctx.rect(-r,-r,r*2,r*2);ctx.fill();ctx.stroke();let dx=Number(u.dirX)||0,dz=Number(u.dirZ)||0;if(dx||dz){ctx.rotate(Math.atan2(dx,dz));ctx.strokeStyle='#f6fbff';ctx.lineWidth=2;ctx.beginPath();ctx.moveTo(0,-r-3);ctx.lineTo(0,-r-15);ctx.moveTo(-4,-r-11);ctx.lineTo(0,-r-15);ctx.lineTo(4,-r-11);ctx.stroke()}ctx.restore()}
function placeNames(){}
function mapPalette(){return darkMap?{sea:'#142631',land:'#1b2429',forest:'#36533a',contour:'#6d6660',waterContour:'#376d87',road:'#b08062',building:'#d1c1ac',symbol:'#d9d5cc'}:{sea:'#c8e6fd',land:'#fffefd',forest:'#cde69a',contour:'#d3baa3',waterContour:'#80c4ff',road:'#7b5c48',building:'#252525',symbol:'#080808'}}
function drawVectorMap(){const m=vectorMap,p=mapPalette(),px=(x,y)=>screen(x/m.scale*m.size,(1-y/m.scale)*m.size),cell=m.scale/m.cells;ctx.fillStyle=p.sea;ctx.fillRect(0,0,c.width,c.height);ctx.fillStyle=p.land;ctx.beginPath();for(const r of m.land){const a=screen(r[1]/m.cells*m.size,(r[0]+1)/m.cells*m.size),b=screen(r[2]/m.cells*m.size,r[0]/m.cells*m.size);ctx.rect(a[0],a[1],b[0]-a[0],b[1]-a[1])}ctx.fill();ctx.fillStyle=p.forest;ctx.beginPath();for(const f of m.forests){const x=f[0]*cell,x1=x+cell,y=m.scale-(f[1]+1)*cell,y1=y+cell,a=px(x,y),b=px(x1,y1);if(f[2]===-128)ctx.rect(a[0],a[1],b[0]-a[0],b[1]-a[1]);else{ctx.moveTo(a[0],a[1]);if(f[2]===-1){const q=px(x,y1),r=px(x1,y);ctx.lineTo(q[0],q[1]);ctx.lineTo(r[0],r[1])}else if(f[2]===0){const q=px(x,y1),r=px(x1,y1);ctx.lineTo(q[0],q[1]);ctx.lineTo(r[0],r[1])}else if(f[2]===1){const q=px(x,y1),r=px(x1,y1),s=px(x1,y);ctx.lineTo(q[0],q[1]);ctx.lineTo(r[0],r[1]);ctx.lineTo(s[0],s[1])}else{const q=px(x1,y1),r=px(x1,y);ctx.lineTo(q[0],q[1]);ctx.lineTo(r[0],r[1])}ctx.closePath()}}ctx.fill();const strokes=(segments,color,width,filter=()=>true)=>{ctx.strokeStyle=color;ctx.lineWidth=width;ctx.beginPath();for(const s of segments){if(!filter(s))continue;const a=px(s[0],s[1]),b=px(s[2],s[3]);ctx.moveTo(a[0],a[1]);ctx.lineTo(b[0],b[1])}ctx.stroke()};strokes(m.contours,p.contour,1,s=>s[4]===0);strokes(m.contours,p.waterContour,1,s=>s[4]===1);strokes(m.roads,p.road,2);strokes(m.buildings,p.building,1);ctx.strokeStyle=p.symbol;ctx.fillStyle=p.symbol;ctx.lineWidth=1.5;ctx.beginPath();const pixelScale=c.width*m.size/(m.scale*spans().x);for(const s of m.symbols){const a=px(s[0],s[1]);if(a[0]<-12||a[1]<-12||a[0]>c.width+12||a[1]>c.height+12)continue;const r=(s[2]===1?4:s[2]===2?3:s[2]===3?1:4)*pixelScale;if(s[2]===4){ctx.moveTo(a[0]-r,a[1]);ctx.lineTo(a[0]+r,a[1]);ctx.moveTo(a[0],a[1]-r);ctx.lineTo(a[0],a[1]+r)}else ctx.moveTo(a[0]+r,a[1]),ctx.arc(a[0],a[1],r,0,Math.PI*2)}ctx.stroke()}
function draw(preferCache=wasDragging&&pointers.size>0){ctx.clearRect(0,0,c.width,c.height);if(vectorMap){const cacheCompatible=mapCacheInfo&&mapCacheInfo.vector===vectorMap&&mapCacheInfo.dark===darkMap&&mapCache.width===c.width&&mapCache.height===c.height;const sameCache=cacheCompatible&&mapCacheInfo.zoom===view.zoom&&Math.abs(mapCacheInfo.cx-view.cx)<.01&&Math.abs(mapCacheInfo.cz-view.cz)<.01;const canPan=preferCache&&cacheCompatible&&mapCacheInfo.zoom===view.zoom;const canZoom=preferCache&&cacheCompatible&&zoomPreview;if(sameCache||canPan||canZoom){ctx.fillStyle=mapPalette().sea;ctx.fillRect(0,0,c.width,c.height);if(canZoom&&mapCacheInfo.zoom!==view.zoom){const scale=view.zoom/mapCacheInfo.zoom,anchor=zoomPreview;ctx.drawImage(mapCache,anchor.x*(1-scale),anchor.y*(1-scale),c.width*scale,c.height*scale)}else{const v=spans(),dx=(mapCacheInfo.cx-view.cx)*c.width/v.x,dy=(view.cz-mapCacheInfo.cz)*c.height/v.z;ctx.drawImage(mapCache,dx,dy)}}else{drawVectorMap();mapCache.width=c.width;mapCache.height=c.height;mapCacheCtx.clearRect(0,0,c.width,c.height);mapCacheCtx.drawImage(c,0,0);mapCacheInfo={vector:vectorMap,dark:darkMap,zoom:view.zoom,cx:view.cx,cz:view.cz}}}else{ctx.fillStyle='#fff';ctx.fillRect(0,0,c.width,c.height);if(terrain.complete&&terrain.naturalWidth){let a=screen(0,state.mapSize),b=screen(state.mapSize,0);ctx.drawImage(terrain,a[0],a[1],b[0]-a[0],b[1]-a[1])}}grid();placeNames();pts=[];for(const u of state.units||[]){if(u.status!=='active')continue;let[x,y]=screen(u.x,u.z);if(x<-24||y<-24||x>c.width+24||y>c.height+24)continue;pts.push([x,y,u]);marker(u,x,y)}document.querySelector('#summary').textContent=`${pts.length}/${(state.units||[]).length} 个存活单位 · 视野 ${Math.round(span())} m · ${Math.round(view.zoom*100)}%`;}
function rows(){let list=(state.units||[]).filter(u=>u.status==='active').slice().sort((a,b)=>a.relation.localeCompare(b.relation)||a.display.localeCompare(b.display));document.querySelector('#units').innerHTML='<table><tr><th>关系</th><th>类型</th><th>位置</th></tr>'+list.map(u=>`<tr data-id="${u.id}"><td style="color:${color(u.relation)}">${esc(u.relation)}</td><td>${esc(u.display||u.type)}</td><td>${Math.round(u.x)}, ${Math.round(u.z)}</td></tr>`).join('')+'</table>';document.querySelectorAll('#units tr[data-id]').forEach(e=>e.onclick=()=>lock(e.dataset.id));}
async function lock(id){let r=await fetch('/api/lock?id='+encodeURIComponent(id),{method:'POST'});document.querySelector('#lock').textContent=JSON.stringify(await r.json(),null,2)}function pick(x,y){let best=null,d=34*34;for(const p of pts){let q=(p[0]-x)**2+(p[1]-y)**2;if(q<d){d=q;best=p[2]}}if(best)lock(best.id)}
function point(e){let r=c.getBoundingClientRect();return{x:(e.clientX-r.left)*c.width/r.width,y:(e.clientY-r.top)*c.height/r.height}}c.addEventListener('wheel',e=>{e.preventDefault();let p=point(e);zoomAt(p.x,p.y,e.deltaY<0?1.18:1/1.18)},{passive:false});c.addEventListener('pointerdown',e=>{let p=point(e);if(!pointers.size){dragStart=p;wasDragging=false;clearTimeout(dragSettleTimer)}pointers.set(e.pointerId,p);c.setPointerCapture(e.pointerId)});c.addEventListener('pointermove',e=>{if(!pointers.has(e.pointerId))return;let p=point(e),old=pointers.get(e.pointerId);pointers.set(e.pointerId,p);if(pointers.size===1){if(dragStart&&Math.hypot(p.x-dragStart.x,p.y-dragStart.y)>7)wasDragging=true;if(wasDragging){let v=spans();view.cx+=(old.x-p.x)/c.width*v.x;view.cz+=(p.y-old.y)/c.height*v.z;clamp();draw(true)}}else if(pointers.size===2){wasDragging=true;let a=[...pointers.values()],d=Math.hypot(a[0].x-a[1].x,a[0].y-a[1].y),mid={x:(a[0].x+a[1].x)/2,y:(a[0].y+a[1].y)/2};if(c._pinch)zoomAt(mid.x,mid.y,d/c._pinch.d);c._pinch={d,mid}}});function release(e){let p=point(e),pickHere=e.type==='pointerup'&&!wasDragging&&pointers.size===1,hadDragged=wasDragging;pointers.delete(e.pointerId);if(pointers.size<2)c._pinch=null;if(!pointers.size){if(pickHere)pick(p.x,p.y);if(hadDragged)dragSettleTimer=setTimeout(()=>draw(false),80);suppressClick=performance.now()+240;dragStart=null}}c.addEventListener('pointerup',release);c.addEventListener('pointercancel',release);c.addEventListener('click',e=>{if(performance.now()<suppressClick)return;let p=point(e);pick(p.x,p.y)});
document.querySelector('#run').onclick=async()=>{let lang=document.querySelector('#lang').value,code=document.querySelector('#code').value,r=await fetch('/api/exec?lang='+lang,{method:'POST',body:code}),text=await r.text();document.querySelector('#result').textContent=text};terrain.onload=draw;
function loadVector(version){fetch('/api/map-vector?v='+version,{cache:'force-cache'}).then(r=>{if(!r.ok)throw Error('vector map unavailable');return r.json()}).then(m=>{if(version===terrainVersion){vectorMap=m;draw()}}).catch(()=>{if(version===terrainVersion){vectorMap=null;terrain.src='/api/map?v='+version}})}
async function refresh(){try{let r=await fetch('/api/state',{cache:'no-store'});state=await r.json();if(state.mapSize!==lastSize){lastSize=state.mapSize;view={cx:state.mapSize/2,cz:state.mapSize/2,zoom:1}}if(state.mapVersion!==terrainVersion){terrainVersion=state.mapVersion;vectorMap=null;loadVector(terrainVersion)}draw();rows()}catch(e){document.querySelector('#summary').textContent='连接失败: '+e}}refresh();setInterval(refresh,500);
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
document.querySelector('.legend').insertAdjacentHTML('afterbegin','<label class="follow-toggle" title="开启后，地图会在每次状态刷新时重新以玩家为中心"><input id="follow-player" type="checkbox">跟踪玩家</label><label class="follow-toggle" title="切换浏览器端矢量地图的深色配色"><input id="dark-map" type="checkbox">深色地图</label>');
const followPlayerControl=document.querySelector('#follow-player'),darkMapControl=document.querySelector('#dark-map');let followPlayer=false;
darkMapControl.addEventListener('change',()=>{darkMap=darkMapControl.checked;vectorTiles.clear();draw()});
function playerUnit(){return(state.units||[]).find(u=>u.isPlayer&&u.status==='active')}
function centerOnPlayer(){const u=playerUnit();if(!u)return;view.cx=u.x;view.cz=u.z}
followPlayerControl.addEventListener('change',()=>{followPlayer=followPlayerControl.checked;if(followPlayer){centerOnPlayer();draw()}});
const drawBeforeFollow=draw;draw=()=>{if(followPlayer)centerOnPlayer();drawBeforeFollow()};
placeNames=()=>{const shown=[];const minDistance=Math.max(72,Math.min(170,span()/90));ctx.save();ctx.font='700 16px Georgia,serif';ctx.textBaseline='middle';for(const place of state.locations||[]){const p=screen(place.x,place.z);if(p[0]<-80||p[1]<-30||p[0]>c.width+80||p[1]>c.height+30)continue;let tooClose=false;for(const other of shown){if((p[0]-other[0])**2+(p[1]-other[1])**2<minDistance**2){tooClose=true;break}}if(tooClose)continue;shown.push(p);ctx.lineWidth=4;ctx.strokeStyle=darkMap?'rgba(10,15,18,.94)':'rgba(255,255,255,.92)';ctx.strokeText(place.name,p[0]+5,p[1]);ctx.fillStyle=darkMap?'#eee5d8':'#151515';ctx.fillText(place.name,p[0]+5,p[1])}ctx.restore()};
const markerBeforeStatus=marker;marker=(u,x,y)=>{markerBeforeStatus(u,x,y);if(String(u.id)===String(state.lockedId||'')){const pulse=.72+.28*(.5+.5*Math.sin(performance.now()/150));ctx.save();ctx.strokeStyle='rgba(67,35,0,.92)';ctx.lineWidth=9;ctx.strokeRect(x-25,y-25,50,50);ctx.strokeStyle=`rgba(255,174,0,${pulse})`;ctx.lineWidth=5;ctx.strokeRect(x-25,y-25,50,50);ctx.restore()}if(u.isPlayer){ctx.save();ctx.strokeStyle='#ffffff';ctx.lineWidth=2;ctx.beginPath();ctx.arc(x,y,16,0,Math.PI*2);ctx.stroke();ctx.restore()}};
setInterval(()=>{if(state.lockedId)draw()},125);
function gridPart(format,value){
  const first=[...format].findIndex(c=>/[0-9A-Za-z]/.test(c));if(first<0)return'';let i=Math.trunc(value),rev='';
  const take=base=>{const q=Math.trunc(i/base),m=i-q*base;i=q;return m};
  for(let p=format.length-1;p>first;--p){const ch=format[p];if(/[0-9]/.test(ch))rev+=String.fromCharCode(48+((take(10)+(ch.charCodeAt(0)-48))%10+10)%10);else if(/[A-J]/.test(ch))rev+=String.fromCharCode(65+((take(10)+(ch.charCodeAt(0)-65))%10+10)%10);else if(/[a-j]/.test(ch))rev+=String.fromCharCode(97+((take(10)+(ch.charCodeAt(0)-97))%10+10)%10);else rev+=ch}
  const ch=format[first];if(/[0-9]/.test(ch))rev+=String.fromCharCode(48+((take(10)+(ch.charCodeAt(0)-48))%10+10)%10);else if(/[A-Z]/.test(ch))rev+=String.fromCharCode(65+((take(26)+(ch.charCodeAt(0)-65))%26+26)%26);else if(/[a-z]/.test(ch))rev+=String.fromCharCode(97+((take(26)+(ch.charCodeAt(0)-97))%26+26)%26);else rev+=ch;
  for(let p=first-1;p>=0;--p)rev+=format[p];return[...rev].reverse().join('')
}
function gameGrid(){const levels=state.grid||[],scale=(state.mapSize?span()/state.mapSize:1);return levels.find(g=>scale<=g.zoomMax)||levels[levels.length-1]}
const genericGrid=grid;grid=()=>{
  const g=gameGrid();if(!g||!g.stepX||!g.stepY){genericGrid();return}const v=spans(),left=view.cx-v.x/2,right=view.cx+v.x/2,bottom=view.cz-v.z/2,top=view.cz+v.z/2,size=state.mapSize||1;
  const startX=Math.ceil((left-g.offsetX)/g.stepX),endX=Math.floor((right-g.offsetX)/g.stepX),startY=Math.ceil((size-top-g.offsetY)/g.stepY),endY=Math.floor((size-bottom-g.offsetY)/g.stepY);
  ctx.save();ctx.strokeStyle=darkMap?'rgba(181,197,180,.42)':'rgba(65,67,50,.58)';ctx.fillStyle=darkMap?'rgba(218,229,214,.94)':'rgba(72,74,56,.95)';ctx.lineWidth=1;ctx.font='700 13px system-ui,sans-serif';ctx.textAlign='center';ctx.textBaseline='top';ctx.beginPath();
  for(let i=startX;i<=endX;i++){const x=i*g.stepX+g.offsetX,a=screen(x,bottom),b=screen(x,top);ctx.moveTo(a[0],a[1]);ctx.lineTo(b[0],b[1])}for(let i=startY;i<=endY;i++){const z=size-(i*g.stepY+g.offsetY),a=screen(left,z),b=screen(right,z);ctx.moveTo(a[0],a[1]);ctx.lineTo(b[0],b[1])}ctx.stroke();
  for(let i=startX;i<=endX;i++){const x=i*g.stepX+g.offsetX,p=screen(x,top),label=gridPart(g.formatX,g.stepX>=0?i:i-1);ctx.fillText(label,p[0],p[1]+3);ctx.textBaseline='bottom';ctx.fillText(label,p[0],c.height-3);ctx.textBaseline='top'}
  ctx.textAlign='left';ctx.textBaseline='middle';for(let i=startY;i<=endY;i++){const z=size-(i*g.stepY+g.offsetY),p=screen(left,z),label=gridPart(g.formatY,g.stepY>=0?i:i-1);ctx.fillText(label,4,p[1]);ctx.textAlign='right';ctx.fillText(label,c.width-4,p[1]);ctx.textAlign='left'}ctx.restore()
};
const vectorTiles=new Map(),tilePixels=256,maxVectorTiles=96;
function vectorTileIndex(m){if(m._tileIndex)return m._tileIndex;const n=16,size=m.scale/n,buckets=Array.from({length:n*n},()=>({c:[],r:[],b:[],f:[],s:[]})),add=(kind,index,x0,y0,x1,y1)=>{const ax=Math.max(0,Math.min(n-1,Math.floor(Math.min(x0,x1)/size))),bx=Math.max(0,Math.min(n-1,Math.floor(Math.max(x0,x1)/size))),ay=Math.max(0,Math.min(n-1,Math.floor(Math.min(y0,y1)/size))),by=Math.max(0,Math.min(n-1,Math.floor(Math.max(y0,y1)/size)));for(let y=ay;y<=by;y++)for(let x=ax;x<=bx;x++)buckets[y*n+x][kind].push(index)};m.contours.forEach((s,i)=>add('c',i,s[0],s[1],s[2],s[3]));m.roads.forEach((s,i)=>add('r',i,s[0],s[1],s[2],s[3]));m.buildings.forEach((s,i)=>add('b',i,s[0],s[1],s[2],s[3]));m.forests.forEach((f,i)=>{const x=f[0]*m.scale/m.cells,y=m.scale-(f[1]+1)*m.scale/m.cells;add('f',i,x,y,x+m.scale/m.cells,y+m.scale/m.cells)});m.symbols.forEach((s,i)=>add('s',i,s[0],s[1],s[0],s[1]));return m._tileIndex={n,size,buckets}}
function tileEntries(index,kind,x0,y0,x1,y1){const a=Math.max(0,Math.min(index.n-1,Math.floor(x0/index.size))),b=Math.max(0,Math.min(index.n-1,Math.floor(x1/index.size))),c0=Math.max(0,Math.min(index.n-1,Math.floor(y0/index.size))),d=Math.max(0,Math.min(index.n-1,Math.floor(y1/index.size))),found=new Set;for(let y=c0;y<=d;y++)for(let x=a;x<=b;x++)for(const entry of index.buckets[y*index.n+x][kind])found.add(entry);return found}
function vectorTile(m,level,tx,ty){const key=`${terrainVersion}:${darkMap?1:0}:${level}:${tx}:${ty}`,old=vectorTiles.get(key);if(old){vectorTiles.delete(key);vectorTiles.set(key,old);return old}const count=1<<level,spanSource=m.scale/count,x0=tx*spanSource,y0=ty*spanSource,x1=x0+spanSource,y1=y0+spanSource,tile=document.createElement('canvas'),d=tile.getContext('2d'),ratio=tilePixels/spanSource,p=mapPalette(),index=vectorTileIndex(m),point=(x,y)=>[(x-x0)*ratio,(y-y0)*ratio],intersects=s=>Math.max(s[0],s[2])>=x0&&Math.min(s[0],s[2])<=x1&&Math.max(s[1],s[3])>=y0&&Math.min(s[1],s[3])<=y1,contourStride=level<=2?8:level===3?4:level===4?2:1,symbolStride=level<=2?12:level===3?6:level===4?3:level===5?2:1;tile.width=tilePixels;tile.height=tilePixels;d.fillStyle=p.sea;d.fillRect(0,0,tilePixels,tilePixels);d.fillStyle=p.land;for(const r of m.land){const rx0=r[1]*m.scale/m.cells,rx1=r[2]*m.scale/m.cells,ry0=m.scale-(r[0]+1)*m.scale/m.cells,ry1=ry0+m.scale/m.cells;if(rx1<x0||rx0>x1||ry1<y0||ry0>y1)continue;const a=point(rx0,ry0),b=point(rx1,ry1);d.fillRect(a[0],a[1],b[0]-a[0],b[1]-a[1])}const cell=m.scale/m.cells;d.fillStyle=p.forest;for(const id of tileEntries(index,'f',x0,y0,x1,y1)){const f=m.forests[id],fx=f[0]*cell,fy=m.scale-(f[1]+1)*cell,a=point(fx,fy),b=point(fx+cell,fy+cell);if(f[2]===-128)d.fillRect(a[0],a[1],b[0]-a[0],b[1]-a[1]);else{d.beginPath();if(f[2]===-1){d.moveTo(a[0],a[1]);d.lineTo(a[0],b[1]);d.lineTo(b[0],a[1])}else if(f[2]===0){d.moveTo(a[0],b[1]);d.lineTo(b[0],b[1]);d.lineTo(a[0],a[1])}else if(f[2]===1){d.moveTo(a[0],b[1]);d.lineTo(b[0],b[1]);d.lineTo(b[0],a[1])}else{d.moveTo(a[0],a[1]);d.lineTo(b[0],b[1]);d.lineTo(b[0],a[1])}d.closePath();d.fill()}}const lines=(kind,items,color,width,stride=1)=>{d.strokeStyle=color;d.lineWidth=width;d.beginPath();for(const id of tileEntries(index,kind,x0,y0,x1,y1)){if(id%stride)continue;const s=items[id];if(!intersects(s))continue;const a=point(s[0],s[1]),b=point(s[2],s[3]);d.moveTo(a[0],a[1]);d.lineTo(b[0],b[1])}d.stroke()};lines('c',m.contours,p.contour,1,contourStride);d.strokeStyle=p.waterContour;d.lineWidth=1;d.beginPath();for(const id of tileEntries(index,'c',x0,y0,x1,y1)){if(id%contourStride)continue;const s=m.contours[id];if(s[4]!==1||!intersects(s))continue;const a=point(s[0],s[1]),b=point(s[2],s[3]);d.moveTo(a[0],a[1]);d.lineTo(b[0],b[1])}d.stroke();lines('r',m.roads,p.road,2);if(level>=3)lines('b',m.buildings,p.building,1);d.strokeStyle=p.symbol;d.lineWidth=1.25;d.beginPath();for(const id of tileEntries(index,'s',x0,y0,x1,y1)){if(id%symbolStride)continue;const s=m.symbols[id],a=point(s[0],s[1]),r=(s[2]===1?4:s[2]===2?3:s[2]===3?1:4)*ratio;if(s[2]===4){d.moveTo(a[0]-r,a[1]);d.lineTo(a[0]+r,a[1]);d.moveTo(a[0],a[1]-r);d.lineTo(a[0],a[1]+r)}else{d.moveTo(a[0]+r,a[1]);d.arc(a[0],a[1],r,0,Math.PI*2)}}d.stroke();vectorTiles.set(key,tile);if(vectorTiles.size>maxVectorTiles)vectorTiles.delete(vectorTiles.keys().next().value);return tile}
function drawVectorTiles(){const m=vectorMap,level=Math.max(2,Math.min(6,Math.round(Math.log2(Math.max(1,view.zoom)))+2)),count=1<<level,srcPerTile=m.scale/count,v=spans(),minX=(view.cx-v.x/2)/m.size*m.scale,maxX=(view.cx+v.x/2)/m.size*m.scale,minY=(1-(view.cz+v.z/2)/m.size)*m.scale,maxY=(1-(view.cz-v.z/2)/m.size)*m.scale,fromX=Math.max(0,Math.floor(minX/srcPerTile)),toX=Math.min(count-1,Math.floor(maxX/srcPerTile)),fromY=Math.max(0,Math.floor(minY/srcPerTile)),toY=Math.min(count-1,Math.floor(maxY/srcPerTile));ctx.fillStyle=mapPalette().sea;ctx.fillRect(0,0,c.width,c.height);for(let y=fromY;y<=toY;y++)for(let x=fromX;x<=toX;x++){const sx=x*srcPerTile,sy=y*srcPerTile,a=screen(sx/m.scale*m.size,(1-sy/m.scale)*m.size),b=screen((sx+srcPerTile)/m.scale*m.size,(1-(sy+srcPerTile)/m.scale)*m.size);ctx.drawImage(vectorTile(m,level,x,y),a[0],a[1],b[0]-a[0],b[1]-a[1])}}
function drawTiledDashboard(){ctx.clearRect(0,0,c.width,c.height);if(vectorMap)drawVectorTiles();else{ctx.fillStyle='#fff';ctx.fillRect(0,0,c.width,c.height);if(terrain.complete&&terrain.naturalWidth){const a=screen(0,state.mapSize),b=screen(state.mapSize,0);ctx.drawImage(terrain,a[0],a[1],b[0]-a[0],b[1]-a[1])}}grid();placeNames();pts=[];for(const u of state.units||[]){if(u.status!=='active')continue;const xy=screen(u.x,u.z);if(xy[0]<-24||xy[1]<-24||xy[0]>c.width+24||xy[1]>c.height+24)continue;pts.push([xy[0],xy[1],u]);marker(u,xy[0],xy[1])}document.querySelector('#summary').textContent=`${pts.length}/${(state.units||[]).length} 个存活单位 · 视野 ${Math.round(span())} m · ${Math.round(view.zoom*100)}%`}
draw=()=>{if(followPlayer)centerOnPlayer();drawTiledDashboard()};
c.addEventListener('pointerup',()=>clearTimeout(dragSettleTimer));c.addEventListener('pointercancel',()=>clearTimeout(dragSettleTimer));zoomAt=(x,y,factor)=>{const p=world(x,y),old=view.zoom;view.zoom=Math.max(1,Math.min(20,view.zoom*factor));if(view.zoom===old)return;const q=world(x,y);view.cx+=p.x-q.x;view.cz+=p.z-q.z;clamp();draw()};
const rowsBeforeGridReference=rows;rows=()=>{rowsBeforeGridReference();document.querySelectorAll('#units tr[data-id]').forEach(row=>{const unit=(state.units||[]).find(u=>String(u.id)===row.dataset.id),cell=row.querySelector('td:last-child');row.classList.toggle('locked-target',!!unit&&String(unit.id)===String(state.lockedId||''));if(unit&&cell&&unit.gridRef)cell.textContent=`${unit.gridRef} (${Math.round(unit.x)}, ${Math.round(unit.z)})`})};
const lockBeforeRowHighlight=lock;lock=async id=>{await lockBeforeRowHighlight(id);try{const result=JSON.parse(document.querySelector('#lock').textContent);if(result.ok){state.lockedId=id;rows();draw()}}catch(_){}};
</script>
)JS";
        const size_t scriptEnd = html.rfind("</script>");
        html.insert(scriptEnd == std::string::npos ? html.size() : scriptEnd + std::strlen("</script>"), additions);
        static constexpr char externalAssets[] = R"HTML(
<link rel="stylesheet" href="/web/dashboard.css">
<script src="/web/dashboard.js"></script>
)HTML";
        const size_t bodyEnd = html.rfind("</body>");
        html.insert(bodyEnd == std::string::npos ? html.size() : bodyEnd, externalAssets);
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
    std::string _terrainVector;
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
            if (_terrainMapSignature == signature && !_terrainMap.empty() && !_terrainVector.empty())
                return;
        }

        // Follow the native CStaticMap's paper-map convention: land is a
        // neutral sheet, while terrain detail is communicated by contours,
        // roads, forest areas and static map symbols.  Sampling the ground
        // texture here made the browser map look like a low-resolution
        // satellite image instead of the game's briefing map.
        // A 4K source keeps map symbols and contour lines legible during the
        // web map's close tactical zoom levels without changing their world
        // size on the overview.
        constexpr int imageSize = 4096;
        const int terrainShift = std::max(0, GLandscape->GetTerrainRangeLog() - GLandscape->GetLandRangeLog());
        const float landSize = landRange * GLandscape->GetLandGrid();
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

        // Pixels are held top-down until the end, while the BMP payload is
        // bottom-up.  The web canvas also defines its top as map north.
        std::vector<uint8_t> pixels(size_t(imageSize) * imageSize * 3);
        struct MapRun { uint16_t z, firstX, lastX; };
        struct MapSegment { uint16_t x0, y0, x1, y1; uint8_t style; };
        struct MapForest { uint16_t x, z; int8_t triangle; };
        struct MapSymbol { uint16_t x, y; uint8_t style; };
        std::vector<MapRun> landRuns;
        std::vector<MapSegment> contourSegments;
        std::vector<MapSegment> roadSegments;
        std::vector<MapSegment> buildingSegments;
        std::vector<MapForest> forests;
        std::vector<MapSymbol> symbols;
        const auto mapCoordinate = [imageSize](int value)
        {
            return static_cast<uint16_t>(std::clamp(value, 0, imageSize));
        };
        auto setPixel = [&pixels](int x, int y, uint8_t red, uint8_t green, uint8_t blue)
        {
            if (x < 0 || y < 0 || x >= imageSize || y >= imageSize)
                return;
            const size_t pixel = (size_t(y) * imageSize + x) * 3;
            pixels[pixel] = blue;
            pixels[pixel + 1] = green;
            pixels[pixel + 2] = red;
        };
        auto blendPixel = [&pixels](int x, int y, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
        {
            if (x < 0 || y < 0 || x >= imageSize || y >= imageSize)
                return;
            const size_t pixel = (size_t(y) * imageSize + x) * 3;
            const int inverse = 255 - alpha;
            pixels[pixel] = static_cast<uint8_t>((pixels[pixel] * inverse + blue * alpha) / 255);
            pixels[pixel + 1] = static_cast<uint8_t>((pixels[pixel + 1] * inverse + green * alpha) / 255);
            pixels[pixel + 2] = static_cast<uint8_t>((pixels[pixel + 2] * inverse + red * alpha) / 255);
        };
        for (int y = 0; y < imageSize; ++y)
        {
            const float landZPosition = float(imageSize - 1 - y) * (landRange - 1) / (imageSize - 1);
            const int landZ = toIntFloor(landZPosition);
            const int terrainZ = std::min(terrainRange - 1, landZ << terrainShift);
            for (int x = 0; x < imageSize; ++x)
            {
                const float landXPosition = float(x) * (landRange - 1) / (imageSize - 1);
                const int landX = toIntFloor(landXPosition);
                const int terrainX = std::min(terrainRange - 1, landX << terrainShift);
                const float height = GLandscape->GetHeight(terrainZ, terrainX);
                if (height < 0.0f)
                {
                    setPixel(x, y, 200, 230, 253);
                }
                else
                    setPixel(x, y, 255, 255, 255);
            }
        }
        // Compact run-length land coverage is enough for the browser to paint
        // the paper/sea base without downloading a raster image.
        for (int landZ = 0; landZ < landRange; ++landZ)
        {
            int landStart = -1;
            for (int landX = 0; landX <= landRange; ++landX)
            {
                const bool isLand = landX < landRange &&
                    GLandscape->GetHeight(std::min(terrainRange - 1, landZ << terrainShift),
                                          std::min(terrainRange - 1, landX << terrainShift)) >= 0.0f;
                if (isLand && landStart < 0)
                    landStart = landX;
                if (!isLand && landStart >= 0)
                {
                    landRuns.push_back({static_cast<uint16_t>(landZ), static_cast<uint16_t>(landStart),
                                        static_cast<uint16_t>(landX)});
                    landStart = -1;
                }
            }
        }
        auto line = [&blendPixel](int x0, int y0, int x1, int y1, uint8_t red, uint8_t green, uint8_t blue, int width)
        {
            const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
            const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
            int error = dx + dy;
            for (;;)
            {
                for (int oy = -width / 2; oy <= width / 2; ++oy)
                    for (int ox = -width / 2; ox <= width / 2; ++ox)
                        blendPixel(x0 + ox, y0 + oy, red, green, blue, 255);
                if (x0 == x1 && y0 == y1)
                    break;
                const int twiceError = 2 * error;
                if (twiceError >= dy)
                    error += dy, x0 += sx;
                if (twiceError <= dx)
                    error += dx, y0 += sy;
            }
        };
        const auto mapX = [landSize](float x) { return toInt(x * imageSize / landSize); };
        const auto mapY = [landSize](float z) { return toInt((landSize - z) * imageSize / landSize); };
        auto fillTriangle = [&blendPixel](int x0, int y0, int x1, int y1, int x2, int y2)
        {
            const int minX = std::max(0, std::min({x0, x1, x2}));
            const int maxX = std::min(imageSize - 1, std::max({x0, x1, x2}));
            const int minY = std::max(0, std::min({y0, y1, y2}));
            const int maxY = std::min(imageSize - 1, std::max({y0, y1, y2}));
            const int area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
            if (area == 0)
                return;
            for (int y = minY; y <= maxY; ++y)
                for (int x = minX; x <= maxX; ++x)
                {
                    const int a = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0);
                    const int b = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1);
                    const int c = (x0 - x2) * (y - y2) - (y0 - y2) * (x - x2);
                    if ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0))
                        blendPixel(x, y, 205, 230, 154, 225);
                }
        };
        auto circle = [&blendPixel](int centerX, int centerY, int radius, uint8_t red, uint8_t green, uint8_t blue)
        {
            const int outer = radius * radius;
            const int inner = std::max(0, radius - 1) * std::max(0, radius - 1);
            for (int y = centerY - radius; y <= centerY + radius; ++y)
                for (int x = centerX - radius; x <= centerX + radius; ++x)
                {
                    const int dx = x - centerX;
                    const int dy = y - centerY;
                    const int distance = dx * dx + dy * dy;
                    if (distance <= outer && distance >= inner)
                        blendPixel(x, y, red, green, blue, 255);
                }
        };
        auto contourTriangle = [&line, &contourSegments, &mapCoordinate](int x0, int y0, float h0, int x1, int y1, float h1, int x2, int y2, float h2, float level)
        {
            int hitX[2] = {};
            int hitY[2] = {};
            int hits = 0;
            auto edge = [&](int ax, int ay, float ah, int bx, int by, float bh)
            {
                if (hits == 2 || !((ah < level && bh >= level) || (bh < level && ah >= level)))
                    return;
                const float range = bh - ah;
                if (std::abs(range) < 0.0001f)
                    return;
                const float t = (level - ah) / range;
                hitX[hits] = toInt(ax + (bx - ax) * t);
                hitY[hits++] = toInt(ay + (by - ay) * t);
            };
            edge(x0, y0, h0, x1, y1, h1);
            edge(x1, y1, h1, x2, y2, h2);
            edge(x2, y2, h2, x0, y0, h0);
            if (hits == 2)
            {
                const bool waterContour = level < 0.0f;
                line(hitX[0], hitY[0], hitX[1], hitY[1], waterContour ? 128 : 211, waterContour ? 196 : 186,
                     waterContour ? 255 : 163, 1);
                contourSegments.push_back({mapCoordinate(hitX[0]), mapCoordinate(hitY[0]), mapCoordinate(hitX[1]),
                                            mapCoordinate(hitY[1]), static_cast<uint8_t>(waterContour ? 1 : 0)});
            }
        };

        // 20 m contours are dense enough to retain the terrain's shape at
        // every zoom level but still practical to rasterise at mission load.
        for (int landZ = 0; landZ < landRange; ++landZ)
            for (int landX = 0; landX < landRange; ++landX)
            {
                const int left = landX * imageSize / landRange;
                const int right = (landX + 1) * imageSize / landRange;
                const int top = imageSize - (landZ + 1) * imageSize / landRange;
                const int bottom = imageSize - landZ * imageSize / landRange;
                const int terrainLeft = std::min(terrainRange - 1, landX << terrainShift);
                const int terrainRight = std::min(terrainRange - 1, (landX + 1) << terrainShift);
                const int terrainTop = std::min(terrainRange - 1, (landZ + 1) << terrainShift);
                const int terrainBottom = std::min(terrainRange - 1, landZ << terrainShift);
                const float hTL = GLandscape->GetHeight(terrainTop, terrainLeft);
                const float hTR = GLandscape->GetHeight(terrainTop, terrainRight);
                const float hBL = GLandscape->GetHeight(terrainBottom, terrainLeft);
                const float hBR = GLandscape->GetHeight(terrainBottom, terrainRight);
                const float lowest = std::min({hTL, hTR, hBL, hBR});
                const float highest = std::max({hTL, hTR, hBL, hBR});
                for (float level = std::floor(lowest / 20.0f) * 20.0f; level <= highest; level += 20.0f)
                {
                    contourTriangle(left, top, hTL, right, top, hTR, left, bottom, hBL, level);
                    contourTriangle(right, top, hTR, right, bottom, hBR, left, bottom, hBL, level);
                }
            }

        // Forest squares are deliberately merged before their outline is
        // drawn.  Outlining every 50 m landscape cell produced a distracting
        // checkerboard instead of the continuous woodland areas in CStaticMap.
        std::vector<uint8_t> forestSquares(size_t(landRange) * landRange, 0);
        for (int landZ = 0; landZ < landRange; ++landZ)
            for (int landX = 0; landX < landRange; ++landX)
            {
                const ObjectList& objects = GLandscape->GetObjects(landZ, landX);
                for (int index = 0; index < objects.Size(); ++index)
                {
                    Object* object = objects[index];
                    LODShapeWithShadow* shape = object ? object->GetShape() : nullptr;
                    if (object && shape && object->GetType() == Primary && shape->GetMapType() == MapForestSquare)
                    {
                        forestSquares[size_t(landZ) * landRange + landX] = 1;
                        break;
                    }
                }
            }
        for (int landZ = 0; landZ < landRange; ++landZ)
            for (int landX = 0; landX < landRange; ++landX)
            {
                const int left = landX * imageSize / landRange;
                const int right = (landX + 1) * imageSize / landRange;
                const int top = imageSize - (landZ + 1) * imageSize / landRange;
                const int bottom = imageSize - landZ * imageSize / landRange;
                const ObjectList& objects = GLandscape->GetObjects(landZ, landX);
                for (int index = 0; index < objects.Size(); ++index)
                {
                    Object* object = objects[index];
                    LODShapeWithShadow* shape = object ? object->GetShape() : nullptr;
                    if (!shape)
                        continue;
                    if (object->GetType() == Primary)
                    {
                        switch (shape->GetMapType())
                        {
                        case MapForestSquare:
                            forests.push_back({static_cast<uint16_t>(landX), static_cast<uint16_t>(landZ), -128});
                            for (int y = top; y < bottom; ++y)
                                for (int x = left; x < right; ++x)
                                    blendPixel(x, y, 205, 230, 154, 225);
                            if (landX == 0 || !forestSquares[size_t(landZ) * landRange + landX - 1])
                                line(left, top, left, bottom, 102, 205, 0, 1);
                            if (landX + 1 == landRange || !forestSquares[size_t(landZ) * landRange + landX + 1])
                                line(right, top, right, bottom, 102, 205, 0, 1);
                            if (landZ == 0 || !forestSquares[size_t(landZ - 1) * landRange + landX])
                                line(left, bottom, right, bottom, 102, 205, 0, 1);
                            if (landZ + 1 == landRange || !forestSquares[size_t(landZ + 1) * landRange + landX])
                                line(left, top, right, top, 102, 205, 0, 1);
                            break;
                        case MapForestTriangle:
                        {
                            const int direction = toInt(std::atan2(object->Direction().X(), object->Direction().Z()) * 2.0f / H_PI);
                            // CStaticMap draws north-up, whereas the legacy
                            // browser triangle codes were derived from the
                            // south-up map export. Serialize the equivalent
                            // north-up triangle code instead of exposing the
                            // raw object rotation.
                            const int8_t webTriangle = direction == -1 ? 0 : direction == 0 ? -1 : direction == 1 ? 2 : 1;
                            forests.push_back({static_cast<uint16_t>(landX), static_cast<uint16_t>(landZ), webTriangle});
                            if (direction == -1)
                                fillTriangle(left, top, left, bottom, right, bottom);
                            else if (direction == 0)
                                fillTriangle(left, top, right, top, left, bottom);
                            else if (direction == 1)
                                fillTriangle(left, top, right, top, right, bottom);
                            else
                                fillTriangle(left, bottom, right, top, right, bottom);
                            if (direction == -1)
                                line(left, top, right, bottom, 102, 205, 0, 1);
                            else if (direction == 0)
                                line(left, bottom, right, top, 102, 205, 0, 1);
                            else if (direction == 1)
                                line(left, top, right, bottom, 102, 205, 0, 1);
                            else
                                line(left, bottom, right, top, 102, 205, 0, 1);
                            break;
                        }
                        case MapTree:
                        {
                            const int x = mapX(object->Position().X()), y = mapY(object->Position().Z());
                            // Match the browser's small-tree marker: dense
                            // woodland remains readable without large circles
                            // obscuring its forest boundary.
                            circle(x, y, 3, 0, 0, 0);
                            symbols.push_back({mapCoordinate(x), mapCoordinate(y), 2});
                            break;
                        }
                        case MapSmallTree:
                        {
                            const int x = mapX(object->Position().X()), y = mapY(object->Position().Z());
                            circle(x, y, 3, 0, 0, 0);
                            symbols.push_back({mapCoordinate(x), mapCoordinate(y), 2});
                            break;
                        }
                        case MapBush:
                        {
                            const int x = mapX(object->Position().X()), y = mapY(object->Position().Z());
                            circle(x, y, 1, 0, 0, 0);
                            symbols.push_back({mapCoordinate(x), mapCoordinate(y), 3});
                            break;
                        }
                        case MapChurch:
                        case MapChapel:
                        case MapCross:
                        {
                            const int x = mapX(object->Position().X());
                            const int y = mapY(object->Position().Z());
                            line(x - 4, y, x + 4, y, 0, 0, 0, 1);
                            line(x, y - 4, x, y + 4, 0, 0, 0, 1);
                            symbols.push_back({mapCoordinate(x), mapCoordinate(y), 4});
                            break;
                        }
                        case MapBuilding:
                        case MapHouse:
                        case MapFence:
                        case MapWall:
                        {
                            const Point3* minmax = shape->MinMax();
                            if (!minmax)
                                break;
                            const Vector3 tl = object->PositionModelToWorld(Point3(minmax[0].X(), 0, minmax[0].Z()));
                            const Vector3 tr = object->PositionModelToWorld(Point3(minmax[1].X(), 0, minmax[0].Z()));
                            const Vector3 bl = object->PositionModelToWorld(Point3(minmax[0].X(), 0, minmax[1].Z()));
                            const Vector3 br = object->PositionModelToWorld(Point3(minmax[1].X(), 0, minmax[1].Z()));
                            const PackedColor color = shape->Color();
                            line(mapX(tl.X()), mapY(tl.Z()), mapX(bl.X()), mapY(bl.Z()), color.R8(), color.G8(), color.B8(), 1);
                            line(mapX(bl.X()), mapY(bl.Z()), mapX(br.X()), mapY(br.Z()), color.R8(), color.G8(), color.B8(), 1);
                            line(mapX(br.X()), mapY(br.Z()), mapX(tr.X()), mapY(tr.Z()), color.R8(), color.G8(), color.B8(), 1);
                            line(mapX(tr.X()), mapY(tr.Z()), mapX(tl.X()), mapY(tl.Z()), color.R8(), color.G8(), color.B8(), 1);
                            const int tlx = mapX(tl.X()), tly = mapY(tl.Z()), trx = mapX(tr.X()), try_ = mapY(tr.Z());
                            const int blx = mapX(bl.X()), bly = mapY(bl.Z()), brx = mapX(br.X()), bry = mapY(br.Z());
                            buildingSegments.push_back({mapCoordinate(tlx), mapCoordinate(tly), mapCoordinate(blx), mapCoordinate(bly), 0});
                            buildingSegments.push_back({mapCoordinate(blx), mapCoordinate(bly), mapCoordinate(brx), mapCoordinate(bry), 0});
                            buildingSegments.push_back({mapCoordinate(brx), mapCoordinate(bry), mapCoordinate(trx), mapCoordinate(try_), 0});
                            buildingSegments.push_back({mapCoordinate(trx), mapCoordinate(try_), mapCoordinate(tlx), mapCoordinate(tly), 0});
                            break;
                        }
                        default:
                            break;
                        }
                    }
                    if (object->GetType() != Network)
                        continue;
                    Point3 ptTL = shape->MemoryPoint("LB");
                    Point3 ptTR = shape->MemoryPoint("PB");
                    Point3 ptBL = shape->MemoryPoint("LE");
                    Point3 ptBR = shape->MemoryPoint("PE");
                    if (!shape->MemoryLevel())
                    {
                        if (Shape* geometry = shape->GeometryLevel())
                        {
                            Vector3Val minimum = geometry->Min();
                            Vector3Val maximum = geometry->Max();
                            ptTL = Vector3(minimum.X(), maximum.Y(), minimum.Z());
                            ptTR = Vector3(maximum.X(), maximum.Y(), minimum.Z());
                            ptBL = Vector3(minimum.X(), maximum.Y(), maximum.Z());
                            ptBR = Vector3(maximum.X(), maximum.Y(), maximum.Z());
                        }
                    }
                    const Vector3 worldTL = object->PositionModelToWorld(ptTL);
                    const Vector3 worldTR = object->PositionModelToWorld(ptTR);
                    const Vector3 worldBL = object->PositionModelToWorld(ptBL);
                    const Vector3 worldBR = object->PositionModelToWorld(ptBR);
                    line(mapX(worldTL.X()), mapY(worldTL.Z()), mapX(worldBL.X()), mapY(worldBL.Z()), 123, 92, 72, 2);
                    line(mapX(worldTR.X()), mapY(worldTR.Z()), mapX(worldBR.X()), mapY(worldBR.Z()), 123, 92, 72, 2);
                    line(mapX(worldTL.X()), mapY(worldTL.Z()), mapX(worldBL.X()), mapY(worldBL.Z()), 212, 182, 150, 1);
                    line(mapX(worldTR.X()), mapY(worldTR.Z()), mapX(worldBR.X()), mapY(worldBR.Z()), 212, 182, 150, 1);
                    roadSegments.push_back({mapCoordinate(mapX(worldTL.X())), mapCoordinate(mapY(worldTL.Z())),
                                            mapCoordinate(mapX(worldBL.X())), mapCoordinate(mapY(worldBL.Z())), 0});
                    roadSegments.push_back({mapCoordinate(mapX(worldTR.X())), mapCoordinate(mapY(worldTR.Z())),
                                            mapCoordinate(mapX(worldBR.X())), mapCoordinate(mapY(worldBR.Z())), 0});
                }
            }
        std::ostringstream vectorJson;
        vectorJson << "{\"size\":" << landSize << ",\"cells\":" << landRange << ",\"scale\":" << imageSize
                   << ",\"land\":[";
        for (size_t index = 0; index < landRuns.size(); ++index)
        {
            const MapRun& run = landRuns[index];
            if (index)
                vectorJson << ',';
            vectorJson << '[' << run.z << ',' << run.firstX << ',' << run.lastX << ']';
        }
        vectorJson << "],\"forests\":[";
        for (size_t index = 0; index < forests.size(); ++index)
        {
            const MapForest& forest = forests[index];
            if (index)
                vectorJson << ',';
            vectorJson << '[' << forest.x << ',' << forest.z << ',' << int(forest.triangle) << ']';
        }
        auto appendSegments = [&vectorJson](const std::vector<MapSegment>& segments)
        {
            for (size_t index = 0; index < segments.size(); ++index)
            {
                const MapSegment& segment = segments[index];
                if (index)
                    vectorJson << ',';
                vectorJson << '[' << segment.x0 << ',' << segment.y0 << ',' << segment.x1 << ',' << segment.y1 << ','
                           << int(segment.style) << ']';
            }
        };
        vectorJson << "],\"contours\":[";
        appendSegments(contourSegments);
        vectorJson << "],\"roads\":[";
        appendSegments(roadSegments);
        vectorJson << "],\"buildings\":[";
        appendSegments(buildingSegments);
        vectorJson << "],\"symbols\":[";
        for (size_t index = 0; index < symbols.size(); ++index)
        {
            const MapSymbol& symbol = symbols[index];
            if (index)
                vectorJson << ',';
            vectorJson << '[' << symbol.x << ',' << symbol.y << ',' << int(symbol.style) << ']';
        }
        vectorJson << "]}";
        for (int bitmapY = 0; bitmapY < imageSize; ++bitmapY)
        {
            const int sourceY = imageSize - 1 - bitmapY;
            const size_t source = size_t(sourceY) * imageSize * 3;
            const size_t destination = 54 + size_t(bitmapY) * rowStride;
            std::memcpy(bitmap.data() + destination, pixels.data() + source, size_t(imageSize) * 3);
        }
        std::lock_guard lock(_mutex);
        _terrainMap = std::move(bitmap);
        _terrainVector = vectorJson.str();
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
        uint64_t lockedId = 0;
        if (AbstractUI* ui = GWorld->UI())
        {
            if (EntityAI* locked = ui->GetLockedTargetForExternal())
                lockedId = EntityKey(locked);
        }
        int mapVersion = 0;
        {
            std::lock_guard lock(_mutex);
            mapVersion = _terrainMapVersion;
        }
        json << "{\"ready\":true,\"mapSize\":" << mapSize << ",\"mapVersion\":" << mapVersion
             << ",\"mode\":" << int(GWorld->GetMode())
             << ",\"gridOffsetX\":" << GWorld->GetGridOffsetX() << ",\"gridOffsetY\":" << GWorld->GetGridOffsetY()
             << ",\"lockedId\":" << lockedId << ",\"grid\":[";
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
        json << "],\"locations\":[";
        bool firstLocation = true;
        const ParamEntry& worldConfig = Pars >> "CfgWorlds" >> Glob.header.worldname;
        const ParamEntry& names = worldConfig >> "Names";
        for (int index = 0; index < names.GetEntryCount(); ++index)
        {
            const ParamEntry& location = names.GetEntry(index);
            const RString name = location >> "name";
            if (name.GetLength() == 0)
                continue;
            const float x = (location >> "position")[0];
            const float z = (location >> "position")[1];
            if (!firstLocation)
                json << ',';
            firstLocation = false;
            json << "{\"name\":\"" << EscapeJson((const char*)name) << "\",\"x\":" << x << ",\"z\":" << z
                 << '}';
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
        if (method == "GET" && (path == "/web/dashboard.css" || path == "/web/dashboard.js"))
        {
            const bool styleSheet = path.ends_with(".css");
            const std::string asset = ReadWebAsset(styleSheet ? "dashboard.css" : "dashboard.js");
            if (asset.empty())
            {
                SendResponse(socket, {404, "application/json; charset=utf-8", JsonError("dashboard asset not found")});
                return;
            }
            SendResponse(socket, {200, styleSheet ? "text/css; charset=utf-8" : "application/javascript; charset=utf-8", asset});
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
        if (method == "GET" && path == "/api/map-vector")
        {
            std::string terrainVector;
            {
                std::lock_guard lock(_mutex);
                terrainVector = _terrainVector;
            }
            if (terrainVector.empty())
                SendResponse(socket, {503, "application/json; charset=utf-8", JsonError("vector terrain map is not ready")});
            else
                SendResponse(socket, {200, "application/json; charset=utf-8", std::move(terrainVector)});
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
