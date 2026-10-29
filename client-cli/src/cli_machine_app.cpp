#include "cli_app.h"
namespace VLan {
void CliApp::machineError(const std::string& code) {
    if(m_machine)m_machine->send("{\"v\":1,\"event\":\"error\",\"requestId\":"+machineQuote(m_machineRequest)+",\"code\":"+machineQuote(code)+"}");
}
void CliApp::machineStatus(const std::string& id) {
    if(!m_machine)return;
    std::ostringstream out;
    out<<"{\"v\":1,\"event\":\"status\",\"requestId\":"<<machineQuote(id)
       <<",\"authenticated\":"<<(m_signal.myPeerId()!=0 && m_signal.isConnected()?"true":"false")
       <<",\"ready\":"<<(m_roomReady?"true":"false")<<",\"roomId\":"<<m_currentRoomId
       <<",\"peerId\":"<<m_signal.myPeerId()<<",\"virtualIp\":"<<machineQuote(ipToString(m_myVirtualIP))
       <<",\"phase\":"<<machineQuote(roomOperationPhaseName(m_roomOperation.phase()))
       <<",\"tcp\":"<<machineQuote(std::to_string(m_tcpPolicy.transportMode))<<",\"udp\":"<<machineQuote(std::to_string(m_udpPolicy.transportMode))<<",\"peers\":[";
    bool first=true;
    for(auto peer:m_tunnel.allPeers()) {
        if(peer->peerId()==m_signal.myPeerId())continue;
        if(!first)out<<",";first=false;
        ProbeQualitySnapshot q=peer->qualitySnapshot(TRAFFIC_TCP);
        out<<"{\"peerId\":"<<peer->peerId()<<",\"virtualIp\":"<<machineQuote(ipToString(peer->virtualIP()))
           <<",\"name\":"<<machineQuote(peer->name())<<",\"rtt\":"<<q.rttMs<<",\"timeoutPercent\":"<<q.timeoutPercent<<",\"sampling\":"<<(q.sampling?"true":"false")
           <<",\"outage\":"<<(q.outage?"true":"false")<<"}";
    }
    out<<"]}";m_machine->send(out.str());
}
void CliApp::pollMachine() {
    if(!m_machine)return;
    if(!m_machine->healthy() && !m_machineStopping) {m_machineStopping=true;requestStop();}
    std::string line;for(int i=0;i<8 && !m_machineStopping && m_machine->poll(line);++i)machineCommand(line);
    uint64_t now=roomMonotonicMs();if(now-m_machineLastStatus>=1000){m_machineLastStatus=now;machineStatus();}
}
void CliApp::machineCommand(const std::string& line) {
    m_machineRequest.clear();
    try {
        auto f=machineParse(line);m_machineRequest=f.at("requestId");
        if(m_machineRequest.empty()||m_machineRequest.size()>64||f.at("v")!="1")throw std::runtime_error("protocol");
        const std::string op=f.at("op");
        if(op=="shutdown") {m_machineStopping=true;requestStop();}
        else if(op=="status"||op=="peers") {machineStatus(m_machineRequest);return;}
        else if(op=="configure") {
            if(m_signal.fd()!=SOCK_INVALID||m_tunJob||m_currentRoomId){machineError("BUSY");return;}
            const auto host=f.at("host"),password=f.at("password"),name=f.at("name");
            uint32_t port=machineNumber(f.at("port"),65535);
            if(!port||host.empty()||host.size()>253||host.find_first_of("\r\n /\\\t")!=std::string::npos||password.size()<8||password.size()>256||!isValidPlayerName(name)||
               std::any_of(host.begin(),host.end(),[](unsigned char c){return c<32;})||std::any_of(password.begin(),password.end(),[](unsigned char c){return c<32;}))throw std::runtime_error("configuration");
            setServer(host,static_cast<uint16_t>(port));setServerPassword(password);setPlayerName(name);doConnect();
        } else if(op=="leave")doLeaveRoom();
        else if(op=="create"||op=="join") {
            if(roomOperationBusy()||m_signal.roomOperationPending()||m_currentRoomId){machineError("BUSY");return;}
            if(!m_signal.isConnected()||!m_signal.myPeerId()){machineError("NOT_AUTHENTICATED");return;}
            auto pass=f.find("roomPassword");std::string password=pass==f.end()?"":pass->second;
            if(!password.empty()&&!isValidRoomPassword(password))throw std::runtime_error("password");
            if(op=="join") {
                uint32_t room=machineNumber(f.at("roomId"),0xffffffffu);if(!room)throw std::runtime_error("room");
                bool found=false;
                for(const auto& item:m_cachedRoomList)if(item.roomId==room){found=true;if(item.passwordProtected && password.empty()){machineError("ROOM_PASSWORD_REQUIRED");return;}break;}
                // The service response remains authoritative; a pushed snapshot may lag.
                (void)found;doJoinRoom(std::to_string(room)+" "+password);
            } else {
                std::string name=f.at("name");if(!isValidPlayerName(name))throw std::runtime_error("room name");
                bool tcp=f.at("compatibility")=="true";
                doCreateRoom(name+" 8 password="+password+(tcp?" tcp=tcp udp=tcp":""));
            }
        } else {machineError("UNKNOWN_OPERATION");return;}
        m_machine->send("{\"v\":1,\"event\":\"accepted\",\"requestId\":"+machineQuote(m_machineRequest)+"}");
    } catch(const std::exception&) {machineError("INVALID_REQUEST");}
}
}
