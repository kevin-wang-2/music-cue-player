#pragma once
#include <functional>
#include <string>

// Wraps an embedded Python 3 interpreter for executing Scriptlet cues.
// Provides an importable 'mcp' module with go / select / alert bindings.
// Only one instance should exist per process (Python initialises globally).
class ScriptletEngine {
public:
    ScriptletEngine();
    ~ScriptletEngine();

    // Callbacks invoked by mcp.go() / mcp.select(cueNum) / mcp.alert(msg).
    void setGoCallback    (std::function<void()>                      cb);
    void setSelectCallback(std::function<void(const std::string&)>    cb);
    void setAlertCallback (std::function<void(const std::string&)>    cb);
    // Called with any text written to stdout/stderr during script execution.
    void setOutputCallback(std::function<void(const std::string&)>    cb);

    // Execute Python code synchronously.  Returns an empty string on success,
    // or a human-readable error / traceback string on failure.
    std::string run(const std::string& code);

private:
    bool m_initialized{false};
};
