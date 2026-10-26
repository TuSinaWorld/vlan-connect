#include "windows_network_config.h"
#include <cassert>
#include <cstdio>

using namespace VLan;

static void expectUtf8(const std::string& value) {
    size_t length = 0;
    assert(windowsValidateUtf8(value, false, &length));
    assert(length == value.size());
}

static WindowsCommandResult decode(const std::string& bytes, unsigned int oem = 936,
                                    unsigned int acp = 1252) {
    WindowsCommandResult result;
    decodeWindowsCommandOutput(bytes, bytes.size(), &result, oem, acp);
    expectUtf8(result.output);
    return result;
}

static void testUnicodeAndLegacyOutput() {
    const std::string chinese = u8"\u786e\u5b9a\u3002\r\n";
    WindowsCommandResult result = decode(chinese);
    assert(result.output == chinese);
    assert(result.outputEncoding == "utf-8" && result.outputCodePage == 65001);
    assert(result.outputCapturedBytes == chinese.size());
    assert(result.outputObservedBytes == chinese.size() && !result.outputTruncated);
    result = decode(std::string("\xef\xbb\xbf") + chinese);
    assert(result.output == chinese && result.outputEncoding == "utf-8-bom");
    result = decode("ASCII success\r\n");
    assert(result.output == "ASCII success\r\n" && result.outputEncoding == "utf-8");

    // BOM-directed UTF-16 includes a surrogate pair, not only BMP characters.
    const char little[] = {char(0xff), char(0xfe), char(0x6e), char(0x78),
        char(0x9a), char(0x5b), char(0x3d), char(0xd8), char(0x00), char(0xde)};
    const char big[] = {char(0xfe), char(0xff), char(0x78), char(0x6e),
        char(0x5b), char(0x9a), char(0xd8), char(0x3d), char(0xde), char(0x00)};
    result = decode(std::string(little, sizeof(little)));
    assert(result.output == u8"\u786e\u5b9a\U0001f600");
    assert(result.outputEncoding == "utf-16le-bom" && result.outputCodePage == 1200);
    result = decode(std::string(big, sizeof(big)));
    assert(result.output == u8"\u786e\u5b9a\U0001f600");
    assert(result.outputEncoding == "utf-16be-bom" && result.outputCodePage == 1201);

#ifdef _WIN32
    const std::string gbk("\xc8\xb7\xb6\xa8\xa1\xa3\r\n");
    result = decode(gbk, 936, 1252);
    assert(result.output == chinese && result.outputEncoding == "oem");
    assert(result.outputCodePage == 936);
    result = decode(std::string(1, char(0xe9)), 65001, 1252);
    assert(result.output == u8"\u00e9" && result.outputEncoding == "acp");
    assert(result.outputCodePage == 1252);
#endif
    result = decode(std::string("\xff\xfe\x00", 3), 65001, 65001);
    assert(result.outputEncoding == "escaped-bytes");
    assert(result.output.find("\\xff\\xfe\\x00") != std::string::npos);
    result = decode(std::string("\xc0\xaf", 2), 65001, 65001); // overlong UTF-8
    assert(result.outputEncoding == "escaped-bytes");
    result = decode(std::string("\xed\xa0\x80", 3), 65001, 65001); // surrogate scalar
    assert(result.outputEncoding == "escaped-bytes");
    result = decode(std::string("\xf4\x90\x80\x80", 4), 65001, 65001); // above U+10FFFF
    assert(result.outputEncoding == "escaped-bytes");
}

static void testBoundedCaptureAndIncompleteTails() {
    const std::string smile = u8"\U0001f600";
    std::string bytes;
    uint64_t observed = 0;
    // Read boundaries may split a single multi-byte character.
    captureWindowsCommandOutput(smile.data(), 1, &bytes, &observed);
    captureWindowsCommandOutput(smile.data() + 1, 2, &bytes, &observed);
    captureWindowsCommandOutput(smile.data() + 3, 1, &bytes, &observed);
    WindowsCommandResult result;
    decodeWindowsCommandOutput(bytes, observed, &result, 936, 1252);
    assert(result.output == smile && !result.outputTruncated);

    const size_t retained = WINDOWS_COMMAND_OUTPUT_LIMIT + WINDOWS_COMMAND_OUTPUT_LOOKAHEAD;
    const std::string payload = std::string(retained - 2, 'a') + smile + "tail";
    bytes.clear(); observed = 0;
    for (size_t i = 0; i < payload.size(); ++i)
        captureWindowsCommandOutput(payload.data() + i, 1, &bytes, &observed);
    assert(bytes.size() == retained && observed == payload.size());
    decodeWindowsCommandOutput(bytes, observed, &result, 936, 1252);
    assert(result.outputEncoding == "utf-8");
    assert(result.output == std::string(retained - 2, 'a'));
    assert(result.outputTruncated && result.outputCapturedBytes == retained);
    assert(result.outputObservedBytes == payload.size());
    expectUtf8(result.output);

    // A process killed during a write can end with an incomplete Unicode tail.
    result = WindowsCommandResult();
    result.exitCode = 123; result.systemError = 5; result.timedOut = true;
    decodeWindowsCommandOutput(std::string("ok\xf0\x9f", 4), 4, &result, 65001, 65001);
    assert(result.output == "ok" && result.outputEncoding == "utf-8");
    assert(result.outputTruncated);
    assert(result.exitCode == 123 && result.systemError == 5 && result.timedOut);
    const char partialUtf16[] = {char(0xff), char(0xfe), 'a', 0, char(0x3d), char(0xd8), 0};
    decodeWindowsCommandOutput(std::string(partialUtf16, sizeof(partialUtf16)),
        sizeof(partialUtf16), &result, 65001, 65001);
    assert(result.output == "a" && result.outputEncoding == "utf-16le-bom");
    assert(result.exitCode == 123 && result.systemError == 5 && result.timedOut);
}

static void testUtf8SafeLogsAndDiagnostics() {
    for (size_t boundary = 2045; boundary <= 2048; ++boundary) {
        const std::string value = std::string(boundary, 'a') + u8"\U0001f600\u786e";
        const std::string logged = windowsLogValue(value);
        expectUtf8(logged);
        assert(logged.size() < 2100);
        assert(logged.find("[truncated]") != std::string::npos);
    }
    assert(windowsLogValue("x\n\"y\r") == "x 'y ");
    expectUtf8(windowsLogValue(std::string(1, char(0xff))));
    WindowsCommandResult result = decode(u8"\u786e\u5b9a\u3002");
    result.parentConsoleCodePage = 65001;
    std::string message;
    WindowsNetworkState state;
    state.alias = L"VLan";
    logWindowsNetwork([&](const std::string& value) { message = value; }, state, "test", 1, result);
    expectUtf8(message);
    assert(message.find("encoding=utf-8 codepage=65001") != std::string::npos);
    assert(message.find("oem_cp=936 acp=1252 parent_console_cp=65001") != std::string::npos);
    assert(message.find("captured_bytes=9 observed_bytes=9 truncated=no") != std::string::npos);
    result.output = std::string(2047, 'a') + u8"\U0001f600";
    logWindowsNetwork([&](const std::string& value) { message = value; }, state, "test", 1, result);
    expectUtf8(message);
    assert(message.find("truncated=yes") != std::string::npos);
    assert(message.find("[truncated]") != std::string::npos);

#ifdef _WIN32
    SystemWindowsNetworkCommands commands;
    result = commands.execute({L"help"}, 10000); // read-only, no elevation or network changes
    assert(result.ok() && !result.output.empty());
    expectUtf8(result.output);
    assert(result.outputCapturedBytes <= WINDOWS_COMMAND_OUTPUT_LIMIT + WINDOWS_COMMAND_OUTPUT_LOOKAHEAD);
    assert(result.outputObservedBytes >= result.outputCapturedBytes);
    assert(!result.outputEncoding.empty());
#endif
}

int main() {
    testUnicodeAndLegacyOutput();
    testBoundedCaptureAndIncompleteTails();
    testUtf8SafeLogsAndDiagnostics();
    std::puts("windows_command_encoding_tests: ok");
    return 0;
}
