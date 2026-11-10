#pragma once

// Public surface of the popout window, used by main.cpp.
// Everything here must be called from foobar2000's main thread.
namespace popout {
    void startup();     // called from initquit::on_init()
    void shutdown();    // called from initquit::on_quit()
    bool is_visible();
    void toggle();
}
