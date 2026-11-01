#include "../ipc.h"

namespace ipc {

void register_sm();
void register_applet();
void register_misc();
void register_fs();
void register_hid();
void register_audio();
void register_gsv();
void register_hwopus();

void register_all_services() {
    register_sm();
    register_applet();
    register_misc();
    register_fs();
    register_hid();
    register_audio();
    register_hwopus();
    register_gsv();  // Switch 2 graphics server: gsv and the syncpt / gpu / vi2 / nvsrv sessions
}

}  // namespace ipc
