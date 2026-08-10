#ifndef VLAN_SERVER_AUTH_FILE_H
#define VLAN_SERVER_AUTH_FILE_H

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace VLan {

inline bool parseAuthPasswordBytes(const std::vector<char>& bytes,
                                   std::string* password,
                                   std::string* error) {
    if (!password || !error) return false;
    password->clear();
    error->clear();
    if (std::find(bytes.begin(), bytes.end(), '\0') != bytes.end()) {
        *error = "auth file contains NUL";
        return false;
    }

    size_t lineEnd = 0;
    while (lineEnd < bytes.size() &&
           bytes[lineEnd] != '\r' && bytes[lineEnd] != '\n') {
        ++lineEnd;
    }
    for (size_t i = lineEnd; i < bytes.size(); ++i) {
        if (bytes[i] != '\r' && bytes[i] != '\n') {
            *error = "auth file contains an extra non-empty line";
            return false;
        }
    }

    password->assign(bytes.begin(), bytes.begin() + lineEnd);
    bool allWhitespace = true;
    for (size_t i = 0; i < password->size(); ++i) {
        const unsigned char ch =
            static_cast<unsigned char>((*password)[i]);
        if (ch != ' ' && ch != '\t' && ch != '\v' && ch != '\f') {
            allWhitespace = false;
            break;
        }
    }
    if (allWhitespace) {
        password->clear();
        *error = "auth password must not be empty or all whitespace";
        return false;
    }
    if (password->size() < 8 || password->size() > 256) {
        password->clear();
        *error = "auth password must be 8-256 bytes";
        return false;
    }
    return true;
}

inline bool readAuthPasswordFile(const std::string& path,
                                 std::string* password,
                                 std::string* error) {
    if (!password || !error) return false;
    password->clear();
    error->clear();
    std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
    if (!in) {
        *error = "cannot read auth file '" + path + "'";
        return false;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    if (in.bad()) {
        *error = "cannot read auth file '" + path + "'";
        return false;
    }
    return parseAuthPasswordBytes(bytes, password, error);
}

} // namespace VLan

#endif // VLAN_SERVER_AUTH_FILE_H
