#ifndef VLAN_CLI_MACHINE_H
#define VLAN_CLI_MACHINE_H
#include <string>
#include <map>
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <limits>
#include <cstdint>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#endif
namespace VLan {
// Deliberately small, bounded JSON object codec for the local machine protocol.
// Requests contain string fields only. No interpolation into shell commands.
inline std::string machineQuote(const std::string& s) {
    std::string out="\""; const char* hex="0123456789abcdef";
    for (unsigned char c : s) {
        if (c=='"' || c=='\\') { out+='\\'; out+=c; }
        else if(c<32) { out+="\\u00"; out+=hex[c>>4]; out+=hex[c&15]; }
        else out+=c;
    }
    return out+'"';
}
inline std::map<std::string,std::string> machineParse(const std::string& line) {
    if(line.size()>16384) throw std::runtime_error("request too large");
    size_t p=0; auto ws=[&](){while(p<line.size() && std::isspace(static_cast<unsigned char>(line[p])))++p;};
    auto str=[&]()->std::string {
        if(p>=line.size() || line[p++]!='"') throw std::runtime_error("string required");
        std::string s; bool done=false;
        while(p<line.size()) {
            unsigned char c=line[p++]; if(c=='"'){done=true;break;}
            if(c<32) throw std::runtime_error("control character");
            if(c=='\\') {
                if(p>=line.size())throw std::runtime_error("escape"); c=line[p++];
                if(c=='u') {
                    auto unit=[&]()->uint32_t {
                        uint32_t n=0;for(int i=0;i<4;++i){if(p>=line.size())throw std::runtime_error("unicode");unsigned char h=line[p++];n<<=4;if(h>='0'&&h<='9')n|=h-'0';else if(h>='a'&&h<='f')n|=h-'a'+10;else if(h>='A'&&h<='F')n|=h-'A'+10;else throw std::runtime_error("unicode");}return n;
                    };
                    uint32_t cp=unit();
                    if(cp>=0xd800 && cp<=0xdbff){if(p+2>line.size()||line[p++]!='\\'||line[p++]!='u')throw std::runtime_error("surrogate");uint32_t low=unit();if(low<0xdc00||low>0xdfff)throw std::runtime_error("surrogate");cp=0x10000+((cp-0xd800)<<10)+low-0xdc00;}
                    else if(cp>=0xdc00 && cp<=0xdfff)throw std::runtime_error("surrogate");
                    if(cp<0x80)s+=static_cast<char>(cp);
                    else if(cp<0x800){s+=static_cast<char>(0xc0|(cp>>6));s+=static_cast<char>(0x80|(cp&63));}
                    else if(cp<0x10000){s+=static_cast<char>(0xe0|(cp>>12));s+=static_cast<char>(0x80|((cp>>6)&63));s+=static_cast<char>(0x80|(cp&63));}
                    else {s+=static_cast<char>(0xf0|(cp>>18));s+=static_cast<char>(0x80|((cp>>12)&63));s+=static_cast<char>(0x80|((cp>>6)&63));s+=static_cast<char>(0x80|(cp&63));}
                    continue;
                }
                if(c=='n')c='\n'; else if(c=='r')c='\r'; else if(c=='t')c='\t';else if(c=='b')c='\b';else if(c=='f')c='\f';
                else if(c!='"' && c!='\\' && c!='/') throw std::runtime_error("unsupported escape");
            }
            s+=c;
        }
        if(!done)throw std::runtime_error("unterminated string"); return s;
    };
    ws(); if(p>=line.size() || line[p++]!='{')throw std::runtime_error("object required");
    std::map<std::string,std::string> fields; ws();
    if(p<line.size() && line[p]=='}')++p;
    else while(true) {
        ws(); std::string key=str(); ws(); if(p>=line.size()||line[p++]!=':')throw std::runtime_error("colon");
        ws(); std::string value;
        if(key=="v" && p<line.size() && line[p]=='1'){value="1";++p;}
        else value=str();
        if(!fields.emplace(key,value).second)throw std::runtime_error("duplicate field");
        if(fields.size()>16)throw std::runtime_error("too many fields");
        ws(); if(p>=line.size())throw std::runtime_error("truncated object");
        char separator=line[p++]; if(separator=='}')break; if(separator!=',')throw std::runtime_error("comma");
    }
    ws(); if(p!=line.size())throw std::runtime_error("trailing data"); return fields;
}
inline uint32_t machineNumber(const std::string& s,uint32_t max) {
    if(s.empty()||s.size()>10)throw std::runtime_error("number required");
    uint64_t n=0;for(char c:s){if(c<'0'||c>'9')throw std::runtime_error("invalid number");n=n*10+c-'0';if(n>max)throw std::runtime_error("number range");}return static_cast<uint32_t>(n);
}
class CliMachine {
public:
    CliMachine(): alive(false)
#ifdef _WIN32
        ,pipe(INVALID_HANDLE_VALUE),parent(nullptr)
#endif
    {}
    ~CliMachine(){
#ifdef _WIN32
        if(pipe!=INVALID_HANDLE_VALUE)CloseHandle(pipe);if(parent)CloseHandle(parent);
#endif
    }
    bool open(const std::string& name,uint32_t parentId) {
#ifdef _WIN32
        if(name.size()!=45||name.substr(0,13)!="PCL-VLAN-IPC-")return false;
        for(size_t i=13;i<name.size();++i)if(!std::isxdigit(static_cast<unsigned char>(name[i])))return false;
        parent=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parentId);if(!parent)return false;
        std::string path="\\\\.\\pipe\\"+name;
        if(!WaitNamedPipeA(path.c_str(),15000))return false;
        pipe=CreateFileA(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        if(pipe==INVALID_HANDLE_VALUE)return false;
        ULONG serverId=0;if(!GetNamedPipeServerProcessId(pipe,&serverId)||serverId!=parentId)return false;
        alive=true;return true;
#else
        (void)name;(void)parentId;return false;
#endif
    }
    bool healthy() {
#ifdef _WIN32
        if(alive && WaitForSingleObject(parent,0)!=WAIT_TIMEOUT)alive=false;
#endif
        return alive;
    }
    bool poll(std::string& line) {
#ifdef _WIN32
        if(!healthy())return false;
        size_t nl=pending.find('\n');
        if(nl==std::string::npos) {
            DWORD available=0;if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr)){alive=false;return false;}
            if(available) { char data[4096];DWORD count=0; if(!transfer(false,data,(std::min)(available,DWORD(sizeof(data))),count))return false;pending.append(data,count); }
            if(pending.size()>16384){alive=false;return false;} nl=pending.find('\n');
        }
        if(nl!=std::string::npos){line=pending.substr(0,nl);pending.erase(0,nl+1);return true;}
#endif
        return false;
    }
    void send(const std::string& json) {
#ifdef _WIN32
        if(!healthy()||json.size()>65535)return;
        std::string data=json+"\n";DWORD count=0;if(!transfer(true,&data[0],static_cast<DWORD>(data.size()),count)||count!=data.size())alive=false;
#endif
    }
private:
    bool alive;std::string pending;
#ifdef _WIN32
    HANDLE pipe,parent;
    bool transfer(bool writing,char* data,DWORD size,DWORD& count) {
        OVERLAPPED ov={};ov.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!ov.hEvent){alive=false;return false;}
        BOOL ok=writing?WriteFile(pipe,data,size,&count,&ov):ReadFile(pipe,data,size,&count,&ov);
        if(!ok && GetLastError()==ERROR_IO_PENDING) {
            HANDLE waits[]={ov.hEvent,parent};DWORD result=WaitForMultipleObjects(2,waits,FALSE,3000);
            if(result==WAIT_OBJECT_0)ok=GetOverlappedResult(pipe,&ov,&count,FALSE);
            else {CancelIoEx(pipe,&ov);GetOverlappedResult(pipe,&ov,&count,TRUE);ok=FALSE;}
        }
        CloseHandle(ov.hEvent);if(!ok)alive=false;return ok!=FALSE;
    }
#endif
};
}
#endif
