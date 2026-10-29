#include "../../client-cli/src/cli_machine.h"
#include <iostream>
#include <stdexcept>
using namespace VLan;
static int count=0;
static void check(bool ok){if(!ok)throw std::runtime_error("machine codec assertion");++count;}
static void reject(const std::string& s){try{machineParse(s);}catch(...){++count;return;}throw std::runtime_error("invalid machine request accepted");}
int main(){
    auto fields=machineParse("{\"v\":\"1\",\"op\":\"configure\",\"requestId\":\"8\",\"password\":\"a\\\"b\\\\c\"}");
    check(fields.at("password")=="a\"b\\c");
    check(machineParse("{\"x\":"+machineQuote("quotes\"slash\\line\n")+"}").at("x")=="quotes\"slash\\line\n");
    check(machineNumber("4294967295",0xffffffffu)==0xffffffffu);
    check(machineNumber("65535",65535)==65535);
    for(auto s:{"", "{} extra", "[]", "{\"op\":1}", "{\"op\":\"a\",\"op\":\"b\"}", "{\"op\":\"a\",}", "{\"op\":\"\\ud800\"}"})reject(s);
    reject(std::string(16385,'a'));
    for(auto s:{"-1","1.0","65536","","999999999999"}){bool bad=false;try{machineNumber(s,65535);}catch(...){bad=true;}check(bad);}
    std::cout<<"PASS: "<<count<<" machine JSON codec checks\n";return 0;
}
