// Host test for src/picker_list.h:  c++ -std=c++17 -I src tests/picker_list_test.cpp && ./a.out
#include <cstdio>
#include <cstring>
#include "picker_list.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static picker_list::List L;

static void card()   // the Lab's card after the six Mutable ports move into /alchemy/mutable
{
    L.Clear();
    const char* top[] = {"smack_alchemy.bin", "benjolin_alchemy.bin", "belt_alchemy.bin", "relay_alchemy.bin",
                         "seq_alchemy.bin", "mark_alchemy.bin", "hypjolin_alchemy.bin", "tally_alchemy.bin",
                         "beads_alchemy.bin", "tether_alchemy.bin", "kerf_alchemy.bin", "pulse_alchemy.bin",
                         "._belt_alchemy.bin", "notes.txt"};
    for (auto n : top) L.AddFile(n, 100);
    const char* mi[] = {"warps_alchemy.bin", "clouds_alchemy.bin", "elements_alchemy.bin",
                        "marbles_alchemy.bin", "meld_alchemy.bin", "plaits_alchemy.bin", "._clouds_alchemy.bin"};
    for (auto n : mi) L.AddIn("mutable", n, 200);
    L.Finish();
}

int main()
{
    card();
    // 12 top files + 6 inside; AppleDouble and .txt skipped
    CHECK(L.nfiles == 18);
    CHECK(L.count == 19);                 // + the folder entry
    // knob at the top: 12 files + 1 folder = 13 dots (fits a 16-dot ring)
    CHECK(L.cur[0] == '\0');
    CHECK(L.nview == 13);
    int folder_slot = -1;
    for (int v = 0; v < L.nview; v++)
        if (L.view[v] >= 0 && L.kind[L.view[v]] == picker_list::kFolder) folder_slot = v;
    CHECK(folder_slot >= 0);
    CHECK(std::strcmp(L.name[L.view[folder_slot]], "mutable") == 0);
    // sorted: kerf before mark before mutable before pulse
    CHECK(std::strcmp(L.name[L.view[folder_slot - 1]], "mark_alchemy.bin") == 0);
    CHECK(std::strcmp(L.name[L.view[folder_slot + 1]], "pulse_alchemy.bin") == 0);

    // open the folder: back dot + 6 files
    L.Enter(L.view[folder_slot]);
    CHECK(std::strcmp(L.cur, "mutable") == 0);
    CHECK(L.nview == 7);
    CHECK(L.view[0] == -1);
    CHECK(std::strcmp(L.ViewLabel(1), "clouds_alchemy.bin") == 0);
    CHECK(std::strcmp(L.ViewLabel(6), "warps_alchemy.bin") == 0);
    CHECK(std::strcmp(L.name[L.view[4]], "mutable/meld_alchemy.bin") == 0);
    CHECK(L.ViewLabel(0)[0] == '\0');

    // a rescan (the app listing) keeps the knob inside the folder
    card();   // Clear() keeps cur
    CHECK(std::strcmp(L.cur, "mutable") == 0);
    CHECK(L.nview == 7);

    // back up
    L.Up();
    CHECK(L.nview == 13);

    // app view: every file by path, sorted
    CHECK(L.FindFile("mutable/plaits_alchemy.bin") >= 0);
    CHECK(L.FindFile("kerf_alchemy.bin") >= 0);
    CHECK(L.FindFile("mutable") < 0);            // a folder is not flashable
    CHECK(L.FindFile("plaits_alchemy.bin") < 0); // only by its path
    CHECK(std::strcmp(L.name[L.files[0]], "beads_alchemy.bin") == 0);
    bool sorted = true;
    for (int k = 1; k < L.nfiles; k++)
        if (strcasecmp(L.name[L.files[k - 1]], L.name[L.files[k]]) > 0) sorted = false;
    CHECK(sorted);

    // folder vanishes from the card: knob falls back to the top
    L.Enter(L.view[folder_slot]);
    L.Clear();
    L.AddFile("belt_alchemy.bin", 1);
    L.Finish();
    CHECK(L.cur[0] == '\0');
    CHECK(L.nview == 1);

    // an empty folder (no .bin) shows no dot; names too long are skipped
    L.Clear();
    L.AddFile("belt_alchemy.bin", 1);
    L.AddIn("empty", "readme.txt", 1);
    L.AddIn("averyveryverylongfoldername", "averyveryverylongfilename_alchemy.bin", 1);
    L.Finish();
    CHECK(L.count == 1);
    CHECK(L.nview == 1);

    // caps: never more than kMaxEntries, never more than kMaxFolders folders
    L.Clear();
    char n[32], d[16];
    for (int i = 0; i < 30; i++) { std::snprintf(n, sizeof n, "f%02d.bin", i); L.AddFile(n, 1); }
    for (int j = 0; j < 9; j++)
        for (int i = 0; i < 3; i++)
        { std::snprintf(d, sizeof d, "d%d", j); std::snprintf(n, sizeof n, "x%d.bin", i); L.AddIn(d, n, 1); }
    L.Finish();
    CHECK(L.count <= picker_list::kMaxEntries);
    int nf = 0;
    for (int i = 0; i < L.count; i++) nf += L.kind[i] == picker_list::kFolder;
    CHECK(nf <= picker_list::kMaxFolders);
    CHECK(L.nview <= picker_list::kMaxView);

    std::printf("%d/%d picker_list checks pass\n", checks - fails, checks);
    return fails ? 1 : 0;
}
