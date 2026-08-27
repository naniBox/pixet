#pragma once

// First-run license acceptance, for platforms that have no installer to present the license.
//
// On Windows the installer shows it and takes acceptance as part of setup (see
// scripts/pixet.iss's LicenseFile), so this is deliberately not reached there: someone who
// already agreed during install must not be asked a second time on first launch. macOS has no
// installer at all - the app is dragged out of a DMG - which leaves first run as the only
// place the license can be put in front of anyone.
namespace license {

// Shows the license modally and blocks until it is answered, unless it has already been
// accepted on this machine, in which case it does nothing and returns true.
//
// Returns false only when the user declines. The caller must then exit without opening a
// window - call this before anything else exists, so that declining leaves no window, no
// database connection and no indexing thread behind.
//
// Acceptance is recorded in the same pixet.ini every other preference lives in (see
// prefs::settingsStore()), so it survives across launches but not across a settings reset.
// That is the intended behaviour rather than an oversight: a fresh config is a fresh first
// run, and re-showing the license is the harmless side of that trade.
bool ensureAccepted();

} // namespace license
