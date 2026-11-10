#include "stdafx.h"
#include "popout_window.h"

DECLARE_COMPONENT_VERSION(
    "Popout Mini Player",
    "1.0.1",
    "A floating, see-through mini player with the track title, artist, a seekable progress bar "
    "and previous / play-pause / next buttons.\n"
    "\n"
    "Show or hide it with View > Popout mini player. You can bind a keyboard shortcut to that "
    "command in Preferences > Keyboard Shortcuts.\n"
    "Drag it anywhere to move it. Right-click it for background opacity, always on top and auto-hide."
);

// Stops the DLL from being renamed or loaded twice.
VALIDATE_COMPONENT_FILENAME("foo_popout_player.dll");

namespace {

    // {6F50B72E-239A-4BE3-9ED1-30369B4BE8A5}
    constexpr GUID guid_cmd_toggle = { 0x6f50b72e, 0x239a, 0x4be3, { 0x9e, 0xd1, 0x30, 0x36, 0x9b, 0x4b, 0xe8, 0xa5 } };

    // View > Popout mini player (checked while the window is showing).
    class popout_mainmenu : public mainmenu_commands {
    public:
        t_uint32 get_command_count() override { return 1; }
        GUID get_command(t_uint32) override { return guid_cmd_toggle; }
        void get_name(t_uint32, pfc::string_base& out) override { out = "Popout mini player"; }
        bool get_description(t_uint32, pfc::string_base& out) override {
            out = "Shows or hides the floating, transparent mini player.";
            return true;
        }
        GUID get_parent() override { return mainmenu_groups::view; }
        bool get_display(t_uint32 index, pfc::string_base& text, t_uint32& flags) override {
            get_name(index, text);
            flags = popout::is_visible() ? flag_checked : 0;
            return true;
        }
        void execute(t_uint32, ctx_t) override { popout::toggle(); }
    };

    class popout_initquit : public initquit {
    public:
        void on_init() override { popout::startup(); }
        void on_quit() override { popout::shutdown(); }
    };

    mainmenu_commands_factory_t<popout_mainmenu> g_mainmenu_factory;
    initquit_factory_t<popout_initquit> g_initquit_factory;
}
