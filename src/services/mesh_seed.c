/**
 * Demo seed — recreates the design's sample mesh so the simulator (and a
 * fresh device with no SD history) shows a populated, believable inbox.
 *
 * Roles drive behavior now: only Clients/Chats are contacts you can message;
 * Routers, Repeaters, Sensors and Rooms are observable nodes without a DM
 * button. Only purposely-known people are seeded as contacts.
 */
#include "mesh.h"
#include <stddef.h>

lz_node_rt *lz_seed_node(uint32_t num, const char *id, lz_net_t net, const char *name,
                         const char *sc, const char *role, float snr, int batt,
                         const char *hw, const char *dist, uint32_t ago_s, bool contact);
void lz_seed_thread(lz_node_rt *n, const char *path, const char *last, uint32_t ago_s,
                    int unread, const lz_msg_rt *history, int hist_n);

void lz_seed_demo(void)
{
    /* --- Meshtastic nodes --- */
    lz_node_rt *ava  = lz_seed_node(0x7c3a91d0, "!7c3a91d0", LZ_NET_MT, "Ava Reyes", "AVA",
                                    "Client", -7.2f, 68, "T-Deck", "1.8 km", 120, true);
    lz_node_rt *base = lz_seed_node(0xa1b2c3d4, "!a1b2c3d4", LZ_NET_MT, "Base-01", "B01",
                                    "Router", 9.5f, 92, "T-Beam", "0.0 km", 5, false);
    lz_seed_node(0x5e6f7a8b, "!5e6f7a8b", LZ_NET_MT, "Summit Relay", "SMT",
                 "Router", 2.1f, 100, "RAK4631", "4.2 km", 60, false);
    lz_node_rt *sam  = lz_seed_node(0x9f21de33, "!9f21de33", LZ_NET_MT, "Sam OK1QRP", "SAM",
                                    "Client", -11.8f, 45, "Heltec V3", "6.7 km", 540, true);
    lz_seed_node(0x1c2d3e4f, "!1c2d3e4f", LZ_NET_MT, "River Cabin", "RVR",
                 "Client", -15.1f, 80, "T-Echo", "11.3 km", 1680, false);

    /* --- MeshCore nodes (present for the inbox/contacts; DMs land in Stage 2) --- */
    lz_node_rt *dmitri = lz_seed_node(0x00004f8e, "MC-4f8e", LZ_NET_MC, "Dmitri K", "DMI",
                                      "Chat", -9.0f, 54, "T-Deck", "2.6 km", 3600, true);
    lz_seed_node(0x00009a3f, "MC-9a3f", LZ_NET_MC, "Ridge Repeater", "RDG",
                 "Repeater", 5.4f, 78, "RAK19007", "3.1 km", 5, false);
    lz_seed_node(0x0000c001, "MC-room", LZ_NET_MC, "Base Camp", "BCR",
                 "Room", 0.0f, -1, "Room Server", "3.1 km", 300, false);
    lz_seed_node(0x0000533e, "MC-sens", LZ_NET_MC, "Weather-Sensor", "WX",
                 "Sensor", 1.2f, 88, "RAK Sensor", "3.0 km", 60, false);

    /* --- conversation histories --- */
    static const lz_msg_rt ava_hist[] = {
        { false, 0, "heading up the summit trail now" },
        { true,  0, "copy, I'm at the basecamp junction" },
        { false, 0, "nice - watch the loose rock past the saddle" },
        { true,  0, "will do. radio check?" },
        { false, 0, "5 by 9, -7 SNR. see you at the ridge in 20" },
    };
    lz_seed_thread(ava, "2 hops", "see you at the ridge in 20", 120, 2,
                   ava_hist, (int)(sizeof ava_hist / sizeof ava_hist[0]));

    static const lz_msg_rt dmitri_hist[] = {
        { false, 0, "trailhead is clear, nobody around" },
        { true,  0, "good. did the repeater come back up?" },
        { false, 0, "yes - ridge repeater is solid now" },
        { false, 0, "sending coords now" },
    };
    lz_seed_thread(dmitri, "3 hops", "sending coords now", 3600, 1,
                   dmitri_hist, (int)(sizeof dmitri_hist / sizeof dmitri_hist[0]));

    static const lz_msg_rt base_hist[] = {
        { false, 0, "copy, staying on LongFast - 73" },
    };
    lz_seed_thread(base, "direct", "copy, staying on LongFast - 73", 840, 0,
                   base_hist, 1);

    static const lz_msg_rt sam_hist[] = {
        { false, 0, "antenna swr looks good now" },
    };
    lz_seed_thread(sam, "1 hop", "antenna swr looks good now", 10800, 0, sam_hist, 1);
}
