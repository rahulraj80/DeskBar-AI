#pragma once
#include <Arduino.h>

#define LAN_MAX_FILES 64

struct LanFileEntry {
    char name[64];
    char url[256];
};

void lan_stream_init(void);
void lan_set_server(const char *ip_port);
const char* lan_get_server(void);
int lan_fetch_files(void);
void lan_fetch_files_async(void);
int lan_get_file_count(void);
const LanFileEntry* lan_get_file(int idx);
bool lan_play(int idx);
extern int lan_track_idx;
