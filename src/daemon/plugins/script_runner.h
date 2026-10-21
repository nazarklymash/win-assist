#pragma once

#include <atomic>
#include <string>

#include "core/manifest.h"
#include "daemon/plugins/action.h"

namespace wa {

// Runs a script plugin: writes one JSON request line to its stdin and reads
// the reply from stdout, enforcing the manifest timeout.
ActionResult run_script_plugin(const PluginManifest& manifest, const std::string& request_json,
                               const std::atomic<bool>& cancel);

}  // namespace wa
