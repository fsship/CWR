#include "DevHttpServer.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <Poseidon/AI/EntityAI.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Game/Scripting/Scripts.hpp>
#include <Poseidon/UI/InGame/InGameUI.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/World.hpp>
#include <Evaluator/express.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>

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
    int objectId = -1;
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
*{box-sizing:border-box}body{margin:0;background:#111820;color:#d9e3ed;font:14px system-ui,sans-serif}header{padding:12px 18px;background:#192631;display:flex;justify-content:space-between;align-items:center}h1{font-size:18px;margin:0}.warn{color:#ffc76a}main{display:grid;grid-template-columns:minmax(400px,2fr) minmax(280px,1fr);gap:12px;padding:12px}.card{background:#18232d;border:1px solid #314351;border-radius:7px;padding:12px}canvas{width:100%;aspect-ratio:1;background:#071016;border:1px solid #425564;cursor:crosshair}.legend span{padding-right:12px}.dot{display:inline-block;width:9px;height:9px;border-radius:99px;margin-right:4px}.west{background:#59a9ff}.east{background:#ff5b62}.guerrila{background:#57d87d}.civilian{background:#fff}.unknown{background:#b8a4ff}textarea{width:100%;height:142px;background:#081016;color:#d9e3ed;border:1px solid #425564;font:12px ui-monospace,monospace;padding:8px}button,select{background:#295d84;color:#fff;border:0;border-radius:4px;padding:7px 10px;margin:4px 0}button:hover{background:#397eaf}pre{white-space:pre-wrap;min-height:32px;color:#a9d5ff}#units{max-height:360px;overflow:auto}table{border-collapse:collapse;width:100%;font-size:12px}th,td{text-align:left;padding:4px;border-bottom:1px solid #2d3c47}tr:hover{background:#263846;cursor:pointer}.dead{opacity:.52}.muted{color:#9caebc}@media(max-width:800px){main{grid-template-columns:1fr}}
</style><body><header><h1>Poseidon LAN 控制台</h1><span class="warn">无认证 · 监听 0.0.0.0:10001</span></header><main>
<section class="card"><canvas id="map" width="900" height="900"></canvas><p class="legend"><span><i class="dot west"></i>West</span><span><i class="dot east"></i>East</span><span><i class="dot guerrila"></i>Resistance</span><span><i class="dot civilian"></i>Civilian</span><span class="muted" id="summary">等待游戏状态…</span></p></section>
<aside><section class="card"><strong>地图锁定</strong><p class="muted">点击地图上的单位，或列表项，尝试以当前玩家所选武器锁定。它仍遵守该单位的正常雷达、可见性与武器锁定规则。</p><pre id="lock">尚未请求</pre></section><section class="card"><strong>脚本执行</strong><p><select id="lang"><option value="sqf">SQF（立即执行）</option><option value="sqs">SQS（排队脚本）</option></select><button id="run">运行</button></p><textarea id="code" spellcheck="false">hint "Hello from Poseidon LAN Control";</textarea><pre id="result"></pre></section><section class="card"><strong>单位</strong><div id="units"></div></section></aside>
</main><script>
let state={ready:false,units:[],mapSize:1};const c=document.querySelector('#map'),ctx=c.getContext('2d');let pts=[];
const color=s=>({west:'#59a9ff',east:'#ff5b62',guerrila:'#57d87d',civilian:'#fff',friendly:'#59a9ff',enemy:'#ff5b62'}[s]||'#b8a4ff');
function draw(){ctx.clearRect(0,0,c.width,c.height);ctx.strokeStyle='#1e3442';ctx.lineWidth=1;for(let i=0;i<=10;i++){let p=i*c.width/10;ctx.beginPath();ctx.moveTo(p,0);ctx.lineTo(p,c.height);ctx.moveTo(0,p);ctx.lineTo(c.width,p);ctx.stroke()}pts=[];let scale=Math.max(1,state.mapSize||1);for(const u of state.units||[]){let x=u.x/scale*c.width,y=c.height-u.z/scale*c.height;if(x<-8||y<-8||x>c.width+8||y>c.height+8)continue;pts.push([x,y,u]);ctx.fillStyle=color(u.side);ctx.globalAlpha=u.status==='destroyed'?.35:1;ctx.beginPath();ctx.arc(x,y,u.kind==='fast'?2.5:4,0,Math.PI*2);ctx.fill()}ctx.globalAlpha=1;document.querySelector('#summary').textContent=`${(state.units||[]).length} 个对象 · 地图 ${Math.round(scale)} m`;}
function rows(){let list=(state.units||[]).slice().sort((a,b)=>a.side.localeCompare(b.side));document.querySelector('#units').innerHTML='<table><tr><th>阵营</th><th>类型</th><th>状态</th><th>位置</th></tr>'+list.map(u=>`<tr class="${u.status==='destroyed'?'dead':''}" data-id="${u.id}"><td>${u.side}</td><td>${u.display||u.type}</td><td>${u.status}</td><td>${Math.round(u.x)}, ${Math.round(u.z)}</td></tr>`).join('')+'</table>';document.querySelectorAll('#units tr[data-id]').forEach(e=>e.onclick=()=>lock(e.dataset.id));}
async function lock(id){let r=await fetch('/api/lock?id='+encodeURIComponent(id),{method:'POST'});document.querySelector('#lock').textContent=JSON.stringify(await r.json(),null,2);}
c.onclick=e=>{let r=c.getBoundingClientRect(),x=(e.clientX-r.left)*c.width/r.width,y=(e.clientY-r.top)*c.height/r.height,best=null,d=15*15;for(const p of pts){let q=(p[0]-x)**2+(p[1]-y)**2;if(q<d){d=q;best=p[2]}}if(best)lock(best.id);};
document.querySelector('#run').onclick=async()=>{let lang=document.querySelector('#lang').value,code=document.querySelector('#code').value,r=await fetch('/api/exec?lang='+lang,{method:'POST',body:code}),text=await r.text();document.querySelector('#result').textContent=text;};
async function refresh(){try{let r=await fetch('/api/state',{cache:'no-store'});state=await r.json();draw();rows()}catch(e){document.querySelector('#summary').textContent='连接失败: '+e}}refresh();setInterval(refresh,500);
</script></body></html>)HTML";
    return page;
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

    void PublishSnapshot()
    {
        if (!GWorld)
            return;

        std::unordered_set<int> seen;
        std::ostringstream json;
        const float mapSize = GLandscape ? GLandscape->GetLandRange() * GLandscape->GetLandGrid() : 0.0f;
        json << "{\"ready\":true,\"mapSize\":" << mapSize << ",\"mode\":" << int(GWorld->GetMode())
             << ",\"units\":[";
        bool first = true;
        auto append = [&](Entity* entity, const char* kind)
        {
            if (!entity || !seen.insert(entity->ID()).second)
                return;
            const Vector3Val pos = entity->Position();
            const Vector3Val speed = entity->Speed();
            const RString display = entity->GetDisplayName();
            const char* status = entity->ToDelete() ? "removing" : (entity->IsDammageDestroyed() ? "destroyed" : "active");
            const EntityAI* ai = dynamic_cast<const EntityAI*>(entity);
            const TargetSide side = ai ? ai->GetTargetSide() : entity->GetTargetSide();
            if (!first)
                json << ',';
            first = false;
            json << "{\"id\":" << entity->ID() << ",\"kind\":\"" << kind << "\",\"type\":\""
                 << EscapeJson(entity->GetName()) << "\",\"display\":\"" << EscapeJson((const char*)display) << "\",\"side\":\""
                 << SideName(side) << "\",\"status\":\"" << status << "\",\"damage\":"
                 << entity->GetTotalDammage() << ",\"x\":" << pos.X() << ",\"y\":" << pos.Y() << ",\"z\":"
                 << pos.Z() << ",\"speed\":" << speed.Size() << '}';
        };
        for (int i = 0; i < GWorld->NVehicles(); ++i) append(GWorld->GetVehicle(i), "vehicle");
        for (int i = 0; i < GWorld->NAnimals(); ++i) append(GWorld->GetAnimal(i), "animal");
        for (int i = 0; i < GWorld->NOutVehicles(); ++i) append(GWorld->GetOutVehicle(i), "person");
        for (int i = 0; i < GWorld->NFastVehicles(); ++i) append(GWorld->GetFastVehicle(i), "fast");
        for (int i = 0; i < GWorld->NBuildings(); ++i) append(GWorld->GetBuilding(i), "building");
        json << "]}";
        std::lock_guard lock(_mutex);
        _snapshot = json.str();
    }

    EntityAI* FindAI(int objectId) const
    {
        if (!GWorld || objectId < 0)
            return nullptr;
        auto find = [objectId](Entity* entity) -> EntityAI*
        {
            if (!entity || entity->ID() != objectId)
                return nullptr;
            return dynamic_cast<EntityAI*>(entity);
        };
        for (int i = 0; i < GWorld->NVehicles(); ++i) if (EntityAI* ai = find(GWorld->GetVehicle(i))) return ai;
        for (int i = 0; i < GWorld->NAnimals(); ++i) if (EntityAI* ai = find(GWorld->GetAnimal(i))) return ai;
        for (int i = 0; i < GWorld->NOutVehicles(); ++i) if (EntityAI* ai = find(GWorld->GetOutVehicle(i))) return ai;
        return nullptr;
    }

    bool TryLock(int objectId) const
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

    HttpResponse AskGame(CommandType type, int objectId, std::string code)
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
        if (method == "POST" && path == "/api/lock")
        {
            const std::string idText = QueryValue(target, "id");
            char* end = nullptr;
            const long id = std::strtol(idText.c_str(), &end, 10);
            if (idText.empty() || !end || *end != 0)
                SendResponse(socket, {400, "application/json; charset=utf-8", JsonError("id must be an object id")});
            else
                SendResponse(socket, AskGame(CommandType::Lock, static_cast<int>(id), {}));
            return;
        }
        if (method == "POST" && path == "/api/exec")
        {
            const std::string language = QueryValue(target, "lang");
            if (language == "sqf")
                SendResponse(socket, AskGame(CommandType::ExecuteSqf, -1, body));
            else if (language == "sqs")
                SendResponse(socket, AskGame(CommandType::ExecuteSqs, -1, body));
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
