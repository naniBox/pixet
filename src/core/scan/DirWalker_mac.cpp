#include "DirWalker.h"

#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "../util/Unicode.h"

namespace pixet {

namespace {

// macOS packages: directories the user experiences as a single document. Walking into one
// is never what anyone wants - a Photos library alone can hold tens of thousands of
// derivative copies of images the user already has elsewhere, and indexing it would bury
// the real library in duplicates. There is no Windows analogue at all, which is why
// DirWalker_win.cpp has nothing like this.
bool isBundleName(const std::string &name) {
    static const char *kBundleExtensions[] = {
        ".app",       ".photoslibrary",        ".imovielibrary", ".musiclibrary", ".tvlibrary",
        ".aplibrary", ".migratedphotolibrary", ".fcpbundle",     ".lrdata",       ".rtfd",
    };
    for (const char *ext : kBundleExtensions) {
        size_t extLen = std::strlen(ext);
        if (name.size() <= extLen) continue;
        // Case-insensitive: the filesystem is, by default, and ".APP" is still a bundle.
        if (strcasecmp(name.c_str() + (name.size() - extLen), ext) == 0) return true;
    }
    return false;
}

} // namespace

// Divergence from DirWalker_win.cpp that is deliberate, not an oversight: this filters
// dot-entries and bundles, and Windows filters nothing (it never checks
// FILE_ATTRIBUTE_HIDDEN). If you're reading the two side by side, that asymmetry is the
// point - the macOS filesystem puts things in every directory that Windows doesn't:
//
//  - .DS_Store in essentially every folder Finder has ever opened, and ._* AppleDouble
//    sidecars for every file copied from a non-Apple volume. Individually inert (they
//    classify as Format::Unknown and get skipped), but they inflate row counts and the
//    folder-freshness diffing for no benefit.
//  - Dot-*directories* are the real problem: .Trash, .Spotlight-V100, .git and friends all
//    get dirs rows and get recursed into. Skipping the whole dot-namespace is simpler and
//    more complete than an extension blocklist, and no photo library depends on dotfiles
//    being indexed.
std::vector<DirEntry> listDir(const std::string &path) {
    std::vector<DirEntry> entries;

    DIR *dir = opendir(path.c_str());
    if (!dir) {
        // Same message shape as the Windows version, including the raw error code. Indexer
        // catches this and skips the directory, so this code is the only surviving clue as
        // to *why* a folder was skipped - and on macOS EACCES from TCC (Documents,
        // Downloads, Desktop, anything under another user's home) is by far the common case.
        throw std::runtime_error("listDir failed (" + std::to_string(errno) + "): " + path);
    }

    int dirFd = dirfd(dir);
    errno = 0;
    while (const struct dirent *ent = readdir(dir)) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        if (!name.empty() && name[0] == '.') continue; // see this function's header comment
        if (isBundleName(name)) continue;

        // AT_SYMLINK_NOFOLLOW is load-bearing rather than merely careful: it makes a
        // symlinked directory report isDir = false, so Indexer never queues it for
        // recursion. That one flag is the primary defense against symlink loops, and macOS
        // ships with several out of the box (/tmp -> /private/tmp, /var, /etc). Windows gets
        // away without an equivalent mostly by luck - reparse points are rare there.
        struct stat st;
        if (fstatat(dirFd, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            // The entry vanished between readdir and stat, or we can't stat it. Skip that
            // one entry rather than failing the whole folder - the same "directories are
            // live, things move mid-walk" reasoning behind readWholeFile returning false
            // instead of throwing.
            errno = 0;
            continue;
        }

        DirEntry entry;
        entry.name = toNfc(name); // see Unicode_mac.cpp - this is not cosmetic
        entry.isDir = S_ISDIR(st.st_mode);
        entry.mtimeUnix = static_cast<int64_t>(st.st_mtimespec.tv_sec); // already Unix seconds
        if (!entry.isDir) entry.size = static_cast<int64_t>(st.st_size);
        entries.push_back(std::move(entry));
        errno = 0;
    }

    // readdir() returns NULL for both "end of directory" and "error", distinguishable only
    // via errno. Checking it matters more than it looks: a silently truncated listing is
    // indistinguishable to Indexer from "every file after this point was deleted", and it
    // will dutifully prune those rows and their thumbnails. DirWalker_win.cpp's
    // `while (FindNextFileW(...))` loop has this exact latent bug - it just hasn't bitten
    // yet, and is worth fixing there on the next Windows session.
    int readdirErrno = errno;
    closedir(dir);
    if (readdirErrno != 0) {
        throw std::runtime_error("listDir failed mid-enumeration (" + std::to_string(readdirErrno) + "): " + path);
    }

    return entries;
}

int64_t dirMtimeUnix(const std::string &path) {
    // stat, not lstat: the caller is asking about the directory it is going to walk, and if
    // the user navigated in via a symlink then the target's mtime is the one that reflects
    // "did the contents change". Also matches GetFileAttributesExW, which follows reparse
    // points.
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        // Includes errno, unlike the Windows version which omits it - same reasoning as
        // listDir. Indexer catches this; uncaught, a single permission-denied folder would
        // abort an entire index run and leak its claim row.
        throw std::runtime_error("dirMtimeUnix failed (" + std::to_string(errno) + "): " + path);
    }
    return static_cast<int64_t>(st.st_mtimespec.tv_sec);
}

} // namespace pixet
