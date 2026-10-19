// Driveclub Native: Finder launch support (see launcher.h).

#include "launcher.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#import <AppKit/AppKit.h>

#include "common/log.h"

namespace Launcher {

namespace {

NSString* const kGameFolderKey = @"GameFolder";

bool g_interactive = false;
std::filesystem::path g_log;
std::filesystem::path g_marker;

std::filesystem::path HomeDir() {
    const char* home = std::getenv("HOME");
    return home ? home : "/tmp";
}

void EnsureApp() {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
}

// Returns the index of the pressed button (0 = first).
NSInteger Alert(NSAlertStyle style, NSString* title, NSString* text, NSArray<NSString*>* buttons) {
    EnsureApp();
    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = style;
    alert.messageText = title;
    alert.informativeText = text;
    for (NSString* b in buttons) {
        [alert addButtonWithTitle:b];
    }
    return [alert runModal] - NSAlertFirstButtonReturn;
}

constexpr size_t kKeepLogs = 20;

// One log per session in "Driveclub Logs" on the Desktop, where people find
// it to attach to a report. The first write there waits until the user
// allows Desktop access, so this runs at launch, never while crashing; if
// access is denied the logs go to ~/Library/Logs/DriveclubNative.
std::filesystem::path OpenSessionLog() {
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", std::localtime(&now));
    const std::string name = std::string("driveclub-") + stamp + ".txt";
    for (const auto& dir : {HomeDir() / "Desktop/Driveclub Logs", HomeDir() / "Library/Logs/DriveclubNative"}) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const auto log = dir / name;
        if (std::freopen(log.c_str(), "w", stderr)) {
            dup2(fileno(stderr), fileno(stdout));
            return log;
        }
    }
    return {};
}

// Keeps the newest kKeepLogs session logs (their names sort by date).
void PruneLogs(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> logs;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const auto name = e.path().filename().string();
        if (name.starts_with("driveclub-") && e.path().extension() == ".txt") {
            logs.push_back(e.path());
        }
    }
    std::sort(logs.begin(), logs.end());
    for (size_t i = 0; i + kKeepLogs < logs.size(); ++i) {
        std::filesystem::remove(logs[i], ec);
    }
}

// Renames driveclub-<date>.txt to driveclub-<date>-<tag>.txt so the sessions
// that went wrong stand out. An open log keeps being written after the rename.
std::filesystem::path TagLog(const std::filesystem::path& log, const char* tag) {
    auto tagged = log;
    tagged.replace_filename(log.stem().string() + "-" + tag + ".txt");
    std::error_code ec;
    std::filesystem::rename(log, tagged, ec);
    return ec ? log : tagged;
}

NSString* LogName(const std::filesystem::path& log) {
    return [NSString stringWithUTF8String:log.filename().c_str()];
}

void RevealLog(const std::filesystem::path& log) {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:log.c_str()]];
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ url ]];
}

bool HasEboot(const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::is_regular_file(dir / "eboot.bin", ec);
}

// The folder itself, or the only folder up to two levels below it holding
// eboot.bin (people often pick the parent of the extracted game).
std::filesystem::path FindGameDir(const std::filesystem::path& picked) {
    if (HasEboot(picked)) {
        return picked;
    }
    std::filesystem::path found;
    int count = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(picked, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (it.depth() >= 1) {
            it.disable_recursion_pending();
        }
        if (it->is_directory(ec) && HasEboot(it->path())) {
            found = it->path();
            ++count;
        }
    }
    return count == 1 ? found : std::filesystem::path{};
}

} // namespace

bool IsInteractive(int argc, char** argv) {
    // Older macOS passes -psn_... when launching an app from Finder.
    return argc < 2 || std::strncmp(argv[1], "-psn", 4) == 0;
}

void BeginSession() {
    g_interactive = true;
    const auto support = HomeDir() / "Library/Application Support/DriveclubNative";
    std::error_code ec;
    std::filesystem::create_directories(support, ec);

    g_log = OpenSessionLog();
    if (!g_log.empty()) {
        PruneLogs(g_log.parent_path());
    }

    // The marker holds the log path of the session in progress.
    g_marker = support / ".running";
    if (std::filesystem::exists(g_marker, ec)) {
        std::string previous;
        std::getline(std::ifstream(g_marker), previous);
        if (!previous.empty() && std::filesystem::exists(previous, ec)) {
            const auto crashed = TagLog(previous, "crash");
            NSString* text = [NSString
                stringWithFormat:@"It may have crashed. Its log is in the \"%@\" folder as %@: "
                                 @"please attach it if you report the problem.",
                                 [NSString stringWithUTF8String:crashed.parent_path().filename().c_str()],
                                 LogName(crashed)];
            if (Alert(NSAlertStyleWarning, @"Driveclub did not close normally last time", text,
                      @[ @"Continue", @"Show Log" ]) == 1) {
                RevealLog(crashed);
            }
        }
    }
    if (std::ofstream marker(g_marker); marker) {
        marker << g_log.string() << '\n';
    }
    // Paths where the game itself calls exit().
    std::atexit(EndSession);
}

void EndSession() {
    if (!g_marker.empty()) {
        std::error_code ec;
        std::filesystem::remove(g_marker, ec);
    }
}

std::filesystem::path ChooseGameFolder() {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    const bool repick = ([NSEvent modifierFlags] & NSEventModifierFlagOption) != 0;
    if (NSString* saved = [defaults stringForKey:kGameFolderKey]; saved && !repick) {
        const std::filesystem::path dir = saved.UTF8String;
        if (HasEboot(dir)) {
            return dir;
        }
    }

    EnsureApp();
    while (true) {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseDirectories = YES;
        panel.canChooseFiles = NO;
        panel.allowsMultipleSelection = NO;
        panel.message = @"Choose your Driveclub v1.28 game folder (the one containing eboot.bin "
                        @"and sce_module).";
        panel.prompt = @"Play";
        if ([panel runModal] != NSModalResponseOK) {
            return {};
        }
        const auto dir = FindGameDir(panel.URL.path.UTF8String);
        if (!dir.empty()) {
            [defaults setObject:[NSString stringWithUTF8String:dir.c_str()] forKey:kGameFolderKey];
            DC_INFO("game folder: %s", dir.c_str());
            return dir;
        }
        const NSInteger choice =
            Alert(NSAlertStyleWarning, @"This is not the Driveclub game folder",
                  @"The folder must contain eboot.bin and sce_module. If the game came as a base "
                  @"game plus a patch, copy the patch files over the base game first.",
                  @[ @"Choose Again", @"Quit" ]);
        if (choice != 0) {
            return {};
        }
    }
}

void ShowError(const char* title, const char* text) {
    if (!g_interactive) {
        return;
    }
    NSString* body = [NSString stringWithUTF8String:text];
    if (g_log.empty()) {
        Alert(NSAlertStyleCritical, [NSString stringWithUTF8String:title], body, @[ @"Quit" ]);
        return;
    }
    std::fflush(stderr);
    g_log = TagLog(g_log, "error");
    body = [body stringByAppendingFormat:@"\n\nThe log is %@.", LogName(g_log)];
    if (Alert(NSAlertStyleCritical, [NSString stringWithUTF8String:title], body,
              @[ @"Quit", @"Show Log" ]) == 1) {
        RevealLog(g_log);
    }
}

void RejectGameFolder(const std::filesystem::path& dir) {
    DC_ERROR("unsupported game version in %s", dir.c_str());
    std::fflush(nullptr);
    if (!g_log.empty()) {
        g_log = TagLog(g_log, "error");
    }
    const NSInteger choice = Alert(
        NSAlertStyleCritical, @"This version of Driveclub is not supported",
        @"The app needs Driveclub v1.28, European release (CUSA00003). Your eboot.bin is a "
        @"different version: the base game without the 1.28 patch, or another region.\n\n"
        @"If you have the 1.28 patch, copy its files over the base game folder (replacing "
        @"eboot.bin), then choose that folder.",
        @[ @"Choose Another Folder", @"Quit" ]);
    EndSession();
    if (choice == 0) {
        [[NSUserDefaults standardUserDefaults] removeObjectForKey:kGameFolderKey];
        [[NSUserDefaults standardUserDefaults] synchronize];
        // Guest modules are already mapped at fixed addresses: start over in a
        // fresh image rather than unload them.
        const char* exe = NSBundle.mainBundle.executablePath.UTF8String;
        char* const args[] = {const_cast<char*>(exe), nullptr};
        execv(exe, args);
    }
    std::_Exit(1);
}

} // namespace Launcher
