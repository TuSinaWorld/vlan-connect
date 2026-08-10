#include "auth_file.h"
#include <cassert>
#include <string>
#include <vector>

using namespace VLan;

static bool parse(const std::vector<char>& bytes, std::string* password) {
    std::string error;
    return parseAuthPasswordBytes(bytes, password, &error);
}

static std::vector<char> chars(const std::string& value) {
    return std::vector<char>(value.begin(), value.end());
}

int main() {
    std::string password;
    assert(parse(chars("12345678"), &password));
    assert(password == "12345678");
    assert(parse(chars("12345678\r\n\n"), &password));
    assert(password == "12345678");
    assert(parse(std::vector<char>(256, 'x'), &password));
    assert(password.size() == 256);

    assert(!parse(std::vector<char>(), &password));
    assert(password.empty());
    assert(!parse(chars("1234567"), &password));
    assert(!parse(std::vector<char>(257, 'x'), &password));
    assert(!parse(chars("        \r\n"), &password));
    assert(!parse(chars("12345678\nnot-empty\n"), &password));
    std::vector<char> nul = chars("12345678");
    nul[3] = '\0';
    assert(!parse(nul, &password));
    return 0;
}
