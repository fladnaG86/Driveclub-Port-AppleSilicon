// Driveclub Native: Finder launch support (see launcher.h).

#include "launcher.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>

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

// Copies the log to ~/Desktop/Driveclub-log.txt, where people find it to
// attach to a report. Only from dialogs: the first write to the Desktop waits
// for the user to grant access, which a dying process must not do.
std::filesystem::path SaveLogToDesktop(const std::filesystem::path& log) {
    const auto dest = HomeDir() / "Desktop/Driveclub-log.txt";
    std::error_code ec;
    std::filesystem::copy_file(log, dest, std::filesystem::copy_options::overwrite_existing, ec);
    return ec ? log : dest;
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
    const auto logs = HomeDir() / "Library/Logs/DriveclubNative";
    const auto support = HomeDir() / "Library/Application Support/DriveclubNative";
    std::error_code ec;
    std::filesystem::create_directories(logs, ec);
    std::filesystem::create_directories(support, ec);

    g_log = logs / "driveclub.log";
    const auto previous = logs / "driveclub-previous.log";
    std::filesystem::rename(g_log, previous, ec);
    if (std::freopen(g_log.c_str(), "w", stderr)) {
        dup2(fileno(stderr), fileno(stdout));
    } else {
        g_log.clear();
    }

    g_marker = support / ".running";
    if (std::filesystem::exists(g_marker, ec)) {
        const auto saved = SaveLogToDesktop(previous);
        const NSInteger choice =
            Alert(NSAlertStyleWarning, @"Driveclub did not close normally last time",
                  @"It may have crashed. Its log was saved to your Desktop as Driveclub-log.txt: "
                  @"please attach it if you report the problem.",
                  @[ @"Continue", @"Show Log" ]);
        if (choice == 1) {
            RevealLog(saved);
        }
    }
    if (FILE* f = std::fopen(g_marker.c_str(), "w")) {
        std::fclose(f);
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
    const auto saved = SaveLogToDesktop(g_log);
    body = [body stringByAppendingString:@"\n\nThe log was saved to your Desktop as Driveclub-log.txt."];
    if (Alert(NSAlertStyleCritical, [NSString stringWithUTF8String:title], body,
              @[ @"Quit", @"Show Log" ]) == 1) {
        RevealLog(saved);
    }
}

void RejectGameFolder(const std::filesystem::path& dir) {
    DC_ERROR("unsupported game version in %s", dir.c_str());
    std::fflush(nullptr);
    if (!g_log.empty()) {
        SaveLogToDesktop(g_log);
    }
    const NSInteger choice = Alert(
        NSAlertStyleCritical, @"This version of Driveclub is not supported",
        @"The app needs Driveclub v1.28, European release (CUSA00003). Your eboot.bin is a "
        @"different version: the base game without the 1.28 patch, or another region.\n\n"
        @"If you have the 1.28 patch, copy its files over the base game folder (replacing "
        @"eboot.bin), then choose that folder.\n\nThe log was saved to your Desktop as "
        @"Driveclub-log.txt.",
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
