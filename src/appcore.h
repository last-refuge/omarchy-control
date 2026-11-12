#pragma once

#include "capabilities.h"
#include "catalog.h"
#include "changecoordinator.h"
#include "commandrunner.h"
#include "compat.h"
#include "env.h"
#include "iconthemeprovider.h"
#include "launchereditor.h"
#include "packageprovider.h"
#include "profilecapture.h"
#include "profileplanner.h"
#include "profilerunner.h"
#include "settingsengine.h"

// The reviewed backend, shared by the window and the headless CLI so both apply
// the same validation, journal, and verification.
struct AppCore {
    Env env;
    Catalog catalog;
    CommandRunner runner;
    Capabilities capabilities;
    ChangeCoordinator coordinator;
    Compat compat;
    IconThemeProvider icons;
    LauncherEditor launchers;
    SettingsEngine engine;
    // My Omarchy: saving this computer's choices to a profile and restoring them.
    PackageProvider packages;
    ProfileCapture profileCapture;
    ProfilePlanner profilePlanner;
    ProfileRunner profileRunner;

    explicit AppCore(const Env &environment, const QString &catalogDirectory = QStringLiteral(":/data"))
        : env(environment),
          catalog(catalogDirectory),
          runner(env),
          capabilities(&runner),
          coordinator(env),
          compat(env, catalogDirectory),
          icons(&runner, &coordinator),
          launchers(env, &coordinator, &icons),
          engine(&catalog, &runner, &capabilities, &coordinator, &compat),
          packages(&runner),
          profileCapture(&catalog, &engine, &capabilities, &packages, env),
          profilePlanner(&catalog, &engine, &capabilities, &packages, env),
          profileRunner(&catalog, &engine, &coordinator, &packages, env) {
        engine.setIconThemes(&icons);
        engine.setLaunchers(&launchers);
    }
};
