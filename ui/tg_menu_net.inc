/* Trap Gunner system menu: the network pages.
 *
 * Included by tg_menu.inc, inside namespace tg_menu. It owns the launcher's
 * three netplay views:
 *
 *   LNG_VIEW_NETPLAY_MODE   LAN or online, and the player name
 *   LNG_VIEW_NETPLAY        the room list: host, join, join by address
 *   LNG_VIEW_LOBBY          the room: players, start, leave
 *
 * Every page works from the D-pad and two buttons. Nothing here asks for a
 * keyboard: the name is picked from the game's cast, a room is hosted with
 * one press, and an address is dialled a field at a time. The stock dialogs
 * (room name and password, typed address, match settings, room mods) are
 * still one line away for anyone with a keyboard, and ALL NETWORK OPTIONS
 * hands the whole thing back to the stock pages.
 *
 * The lobby backend is driven through the same np_* helpers the stock pages
 * use, in the same order each frame, so the two are interchangeable.
 *
 * Expected from the including file, beyond what tg_menu.inc lists: np_cb()
 * and the np_* helpers, LobbySnapshot, launcher_udp_port.h, and
 * tg_menu_lobby_popups().
 */

/* The cast. A new player is given one of these as a name. */
static const char* const kCallsigns[] = {
    "Van Raily", "John Bishous", "Tico", "Lou Riche", "Abdoll Relin",
    "Tenrou Ugetsu",
};
static const int kCallsignCount = (int)(sizeof kCallsigns / sizeof kCallsigns[0]);

static void set_callsign(LauncherModel* m, const char* name) {
    /* What the stock name dialog does on Save. */
    char previous_default[96];
    std::snprintf(previous_default, sizeof previous_default, "%s's Lobby",
                  m->s.netplay_player_name);
    const bool rename_room = !m->netplay_host_name[0] ||
        std::strcmp(m->netplay_host_name, previous_default) == 0;
    std::snprintf(m->s.netplay_player_name, sizeof m->s.netplay_player_name,
                  "%s", name);
    if (rename_room)
        std::snprintf(m->netplay_host_name, sizeof m->netplay_host_name,
                      "%s's Lobby", np_effective_name(m));
    const auto* np = np_cb(m);
    if (np && np->set_player_name)
        np->set_player_name(np->ctx, m->s.netplay_player_name);
}

static int callsign_index(const LauncherModel* m) {
    for (int i = 0; i < kCallsignCount; ++i)
        if (std::strcmp(kCallsigns[i], m->s.netplay_player_name) == 0) return i;
    return -1;
}

static void step_callsign(LauncherModel* m, int dir) {
    int i = callsign_index(m);
    if (i < 0) i = dir > 0 ? -1 : 0;
    i = (i + (dir > 0 ? 1 : kCallsignCount - 1)) % kCallsignCount;
    set_callsign(m, kCallsigns[i]);
}

/* Once per run, and a name for anyone who has none: the stock page asks for
 * a typed name before it shows anything. */
static void net_setup(LauncherModel* m) {
    if (!g_state.net_ready) {
        g_state.net_ready = true;
        np_load_network_settings(m);
        np_pin_host_ice_relay(m);
        np_refresh_host_ip(m);
    }
    if (!np_effective_name(m)[0]) {
        const unsigned pick = (unsigned)(ImGui::GetTime() * 100000.0) +
                              (unsigned)ImGui::GetFrameCount() * 7919u;
        set_callsign(m, kCallsigns[pick % (unsigned)kCallsignCount]);
    }
    m->netplay_name_prompted = true;
}

/* What every netplay page owes the backend each frame. */
static void net_tick(LauncherModel* m, const RecompLauncherCNetplayCallbacks* np) {
    if (np->pump) np->pump(np->ctx);
    np_ingest_last_error(m, np);
    if (np->launch_pending && np->launch_pending(np->ctx))
        np_try_launch(m);
}

static bool address_usable(const char* ip) {
    return ip && ip[0] && std::strcmp(ip, "Unavailable") != 0 &&
           std::strcmp(ip, "Detecting...") != 0;
}

static const char* disc_complaint(const LauncherModel* m) {
    return m->verify.netplay_detail[0] ? m->verify.netplay_detail
                                       : "The game disc is needed for network play.";
}

/* ---- mode page ------------------------------------------------------------- */

enum ModeRow { MODE_LAN = 0, MODE_ONLINE, MODE_CALLSIGN, MODE_BACK, MODE__COUNT };

static void draw_net_mode(LauncherModel* m, const Gfx& g, const Input& in) {
    const auto* np = np_cb(m);
    if (!np) { launcher_model_set_view(m, LNG_VIEW_DASHBOARD); return; }
    net_setup(m);
    if (np->pump) np->pump(np->ctx);

    /* Online waits a moment for a saved sign-in to answer, as the stock page
     * does, so nobody is sent to sign in again by pressing too early. */
    const bool offered = np_account_offered(m);
    const int acct = np->account_state ? np->account_state(np->ctx)
                                       : RECOMP_LAUNCHER_ACCOUNT_GUEST;
    const double now = ImGui::GetTime();
    const bool awaiting = offered && acct == RECOMP_LAUNCHER_ACCOUNT_WAITING;
    if (!awaiting) g_state.signin_since = 0.0;
    else if (g_state.signin_since <= 0.0) g_state.signin_since = now;
    const bool hold_online = awaiting && now - g_state.signin_since <= 5.0;
    const bool signed_in = np_account_signed_in(m);

    move_cursor(PAGE_NET_MODE, MODE__COUNT, in, MODE__COUNT);
    int& cur = g_state.cursor[PAGE_NET_MODE];

    bezel(g, "NETWORK PLAY");
    const std::string callsign = upper(np_effective_name(m));
    const char* labels[MODE__COUNT] = { "LAN PLAY", "ONLINE PLAY", "NAME", "EXIT" };
    int activate = -1;
    for (int i = 0; i < MODE__COUNT; ++i) {
        bool hovered = false;
        const bool enabled = !(i == MODE_ONLINE && hold_online) &&
                             !(i == MODE_CALLSIGN && signed_in);
        if (row(g, kListX, kListY + kRowH * i, kValueR, i == cur, enabled, labels[i],
                i == MODE_CALLSIGN ? callsign.c_str() : nullptr, &hovered))
            activate = i;
        if (hover_moved(hovered)) cur = i;
    }
    if (in.confirm) activate = cur;

    {
        float y = kListY + kRowH * MODE__COUNT + 22.0f;
        char buf[160];
        const bool disc = launcher_model_netplay_disc_ok(m);
        const float x0 = kListX, x1 = 280.0f;
        status_line(g, y, "DISC", disc ? kWhite : kRed, disc ? "READY" : "NOT READY", x0, x1);
        status_line(g, y, "ADDRESS", kWhite,
                    address_usable(m->netplay_host_ip) ? m->netplay_host_ip : "NOT FOUND",
                    x0, x1);
        std::snprintf(buf, sizeof buf, "%u / 15 BLOCKS",
                      (unsigned)launcher_model_memcard_blocks_used(m, 0));
        status_line(g, y, "MEMORY CARD", kWhite, buf, x0, x1);
        if (offered)
            status_line(g, y, "ACCOUNT", kWhite,
                        signed_in ? "SIGNED IN" : awaiting ? "CHECKING" : "GUEST", x0, x1);
    }

    static const char* const captions[MODE__COUNT] = { "LAN", "ONLINE", "NAME", "EXIT" };
    static const char* const info[MODE__COUNT] = {
        "Play someone on the same network, or connect straight to an address "
        "they give you. Rooms on your network appear by themselves.",
        "Find other players on the lobby server.",
        "The name other players see. Left and right pick one of the cast; "
        "press the button to type your own.",
        "Back to the system menu.",
    };
    const char* body = (cur == MODE_ONLINE && hold_online) ? "Checking your saved sign-in."
                     : (cur == MODE_CALLSIGN && signed_in)
                           ? "You are signed in, so your account name is used."
                           : info[cur];
    if (!launcher_model_netplay_disc_ok(m) && cur != MODE_CALLSIGN && cur != MODE_BACK)
        info_plate(g, 304.0f, 288.0f, 290.0f, 110.0f, captions[cur], disc_complaint(m), kRed);
    else
        info_plate(g, 304.0f, 288.0f, 290.0f, 110.0f, captions[cur], body);
    hints(g, "SELECT", "EXIT", "LEFT/RIGHT:NAME");

    if (cur == MODE_CALLSIGN && !signed_in && activate < 0) {
        if (in.left)  step_callsign(m, -1);
        if (in.right) step_callsign(m, 1);
    }
    if (in.back) activate = MODE_BACK;
    if (activate < 0) return;
    if (activate != MODE_BACK) cur = activate;
    switch (activate) {
        case MODE_LAN:
            g_state.next_list = 0.0;
            np_enter_netplay(m, 1);
            break;
        case MODE_ONLINE:
            if (hold_online) break;
            g_state.next_list = 0.0;
            if (!offered || signed_in) np_enter_netplay(m, 2);
            else launcher_model_set_view(m, LNG_VIEW_NETPLAY_SIGNIN);
            break;
        case MODE_CALLSIGN:
            if (!signed_in) np_open_name_modal(m);
            break;
        default:
            launcher_model_set_view(m, LNG_VIEW_DASHBOARD);
            break;
    }
}

/* ---- hosting and joining ----------------------------------------------------- */

/* One press: a room under the default name, no password, on the first free
 * port. The stock Host Lobby dialog's Create, without the dialog. */
static void host_room(LauncherModel* m) {
    const auto* np = np_cb(m);
    if (!np || !np->create) return;
    if (!launcher_model_netplay_disc_ok(m)) {
        std::snprintf(m->netplay_status, sizeof m->netplay_status, "%s",
                      disc_complaint(m));
        return;
    }
    m->netplay_lan_only = (m->netplay_mode == 1);
    if (!m->netplay_lan_only) np_connect_and_list(m);
    np_refresh_host_ip(m);
    if (!address_usable(m->netplay_host_ip)) {
        std::snprintf(m->netplay_status, sizeof m->netplay_status,
                      "No network address found. Check the connection.");
        return;
    }
    if (!m->netplay_host_name[0] || np_name_refused(m, m->netplay_host_name))
        std::snprintf(m->netplay_host_name, sizeof m->netplay_host_name,
                      "%s's Lobby", np_effective_name(m));
    m->netplay_host_password[0] = '\0';

    char endpoint[96];
    const char* port_label = np_valid_port(m->netplay_host_port)
                                 ? m->netplay_host_port : "7777";
    std::snprintf(endpoint, sizeof endpoint, "%s:%s", m->netplay_host_ip, port_label);
    const int want_port = launcher_endpoint_port(endpoint);
    const int free_port = launcher_udp_find_free_port(want_port, 32);
    if (free_port < 0 ||
        (free_port != want_port &&
         launcher_endpoint_set_port(endpoint, sizeof endpoint, free_port) != 0)) {
        std::snprintf(m->netplay_status, sizeof m->netplay_status,
                      "No free network port near %s. Try again.", port_label);
        return;
    }
    const int max_slots = np_clamp_host_max_players(m);
    if (np->rollback_set)
        (void)np->rollback_set(np->ctx, m->netplay_rollback ? 1 : 0);
    if (np->input_prediction_set)
        (void)np->input_prediction_set(np->ctx, m->netplay_lobby_input_prediction);
    if (np->input_delay_set)
        (void)np->input_delay_set(np->ctx, m->netplay_lobby_input_delay);
    const int rc = np->create(np->ctx, m->netplay_host_name, endpoint,
                              m->netplay_host_password, &m->s,
                              m->netplay_lan_only ? 1 : 0, max_slots);
    if (rc != 0) {
        std::snprintf(m->netplay_status, sizeof m->netplay_status,
                      rc == -4 ? "That network port is in use. Try again."
                               : "Could not open a room.");
        return;
    }
    if (const char* colon = std::strrchr(endpoint, ':'))
        std::snprintf(m->netplay_host_port, sizeof m->netplay_host_port, "%s",
                      colon + 1);
    std::snprintf(m->netplay_host_endpoint, sizeof m->netplay_host_endpoint, "%s",
                  endpoint);
    std::snprintf(m->netplay_host_local_ip, sizeof m->netplay_host_local_ip, "%s",
                  m->netplay_host_ip);
    np_save_network_settings(m);
    m->netplay_lobby_max_slots = max_slots;
    m->netplay_local_room = m->netplay_lan_only || !np_connected(m);
    m->netplay_status[0] = '\0';
}

/* The stock Host Lobby dialog, for a room name or a password. */
static void host_room_dialog(LauncherModel* m) {
    if (!launcher_model_netplay_disc_ok(m)) {
        std::snprintf(m->netplay_status, sizeof m->netplay_status, "%s",
                      disc_complaint(m));
        return;
    }
    m->netplay_lan_only = (m->netplay_mode == 1);
    if (!m->netplay_lan_only) np_connect_and_list(m);
    np_refresh_host_ip(m);
    if (!m->netplay_host_name[0])
        std::snprintf(m->netplay_host_name, sizeof m->netplay_host_name,
                      "%s's Lobby", np_effective_name(m));
    m->netplay_host_password[0] = '\0';
    m->netplay_host_modal_open = true;
}

/* ---- join by address ----------------------------------------------------------- */

static void direct_open(LauncherModel* m) {
    int a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(m->netplay_direct_ip, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
        /* Start from this machine's own network: usually only the last
         * number differs. */
        if (std::sscanf(m->netplay_host_ip, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
            a = 192; b = 168; c = 1;
        }
        d = 1;
    }
    const int ip[4] = { a, b, c, d };
    for (int i = 0; i < 4; ++i)
        g_state.direct_ip[i] = ip[i] < 0 ? 0 : ip[i] > 255 ? 255 : ip[i];
    g_state.direct_port = np_valid_port(m->netplay_direct_port)
                              ? std::atoi(m->netplay_direct_port) : 7777;
    g_state.direct_field = 3;
    g_state.direct = true;
    m->netplay_status[0] = '\0';
}

static void direct_join(LauncherModel* m) {
    const auto* np = np_cb(m);
    if (!np || !np->join) return;
    if (!launcher_model_netplay_disc_ok(m)) {
        std::snprintf(m->netplay_status, sizeof m->netplay_status, "%s",
                      disc_complaint(m));
        return;
    }
    std::snprintf(m->netplay_direct_ip, sizeof m->netplay_direct_ip, "%d.%d.%d.%d",
                  g_state.direct_ip[0], g_state.direct_ip[1],
                  g_state.direct_ip[2], g_state.direct_ip[3]);
    std::snprintf(m->netplay_direct_port, sizeof m->netplay_direct_port, "%d",
                  g_state.direct_port);
    char lobby_id[96];
    char guest_bind[64];
    std::snprintf(lobby_id, sizeof lobby_id, "lan:%s:%s", m->netplay_direct_ip,
                  m->netplay_direct_port);
    if (!np_prepare_guest_bind(guest_bind, sizeof guest_bind, m->netplay_status,
                               sizeof m->netplay_status))
        return;
    const int rc = np->join(np->ctx, lobby_id, "", guest_bind);
    if (rc == 0) {
        m->netplay_local_room = true;
        std::snprintf(m->netplay_host_endpoint, sizeof m->netplay_host_endpoint,
                      "%s", lobby_id + 4);
        m->netplay_lobby_max_slots = np_game_max_players(m, true);
        m->netplay_status[0] = '\0';
        g_state.direct = false;
    } else if (rc == -2) {
        std::snprintf(m->netplay_status, sizeof m->netplay_status,
                      "That room has a password. Press X to type it.");
    } else if (rc == -3) {
        std::snprintf(m->netplay_status, sizeof m->netplay_status,
                      "No answer from that address. Check that the host has a "
                      "room open and the numbers are right.");
    } else {
        std::snprintf(m->netplay_status, sizeof m->netplay_status,
                      "Could not join: the room is full, already playing, or "
                      "running a different version.");
    }
}

static void draw_net_direct(LauncherModel* m, const Gfx& g, const Input& in) {
    int& field = g_state.direct_field;
    if (in.left)  field = (field + 4) % 5;
    if (in.right) field = (field + 1) % 5;
    int step = 0;
    if (in.up) step = 1;
    if (in.down) step = -1;
    if (in.page_up) step = 10;
    if (in.page_down) step = -10;
    if (step != 0) {
        if (field < 4) {
            int& v = g_state.direct_ip[field];
            v = ((v + step) % 256 + 256) % 256;
        } else {
            int& v = g_state.direct_port;
            v += step * (step == 10 || step == -10 ? 10 : 1);   /* 1 or 100 */
            if (v < 1) v = 65535;
            if (v > 65535) v = 1;
        }
    }

    bezel(g, "JOIN BY ADDRESS");
    {
        /* One line of five fields, centred; the chosen one in blue with
         * arrows, like the game's TIME LIMIT digits. */
        const float size = 30.0f;
        char part[5][8];
        for (int i = 0; i < 4; ++i)
            std::snprintf(part[i], sizeof part[i], "%03d", g_state.direct_ip[i]);
        std::snprintf(part[4], sizeof part[4], "%d", g_state.direct_port);
        const float dot_w = text_width(g, size, " . ");
        const float colon_w = text_width(g, size, " : ");
        float total = dot_w * 3.0f + colon_w;
        for (int i = 0; i < 5; ++i) total += text_width(g, size, part[i]);
        float x = (kW - total) * 0.5f;
        const float y = 118.0f;
        for (int i = 0; i < 5; ++i) {
            const float w = text_width(g, size, part[i]);
            const bool sel = i == field;
            const ImVec2 a = at(g, x - 6.0f, y - 6.0f), b = at(g, x + w + 6.0f, y + size + 6.0f);
            if (ImGui::IsMouseHoveringRect(a, b, false) &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                field = i;
            if (sel) {
                g.dl->AddRectFilled(a, b, kBlueBand, 3.0f * g.s);
                const float cx = x + w * 0.5f;
                g.dl->AddTriangleFilled(at(g, cx - 6.0f, y - 12.0f),
                                        at(g, cx + 6.0f, y - 12.0f),
                                        at(g, cx, y - 20.0f), kRed);
                g.dl->AddTriangleFilled(at(g, cx - 6.0f, y + size + 12.0f),
                                        at(g, cx + 6.0f, y + size + 12.0f),
                                        at(g, cx, y + size + 20.0f), kRed);
            }
            text(g, x, y, size, sel ? kBlue : kGreen, part[i]);
            x += w;
            if (i < 3) { text(g, x, y, size, kWhite, " . "); x += dot_w; }
            else if (i == 3) { text(g, x, y, size, kWhite, " : "); x += colon_w; }
        }
        text(g, kListX, 190.0f, 10.0f, kGrey, "ADDRESS");
        text_right(g, kValueR, 190.0f, 10.0f, kGrey, "PORT");
    }

    {
        float y = 216.0f;
        if (address_usable(m->netplay_host_ip))
            status_line(g, y, "THIS MACHINE", kWhite, m->netplay_host_ip, kListX, 280.0f);
        if (m->netplay_status[0])
            text(g, kListX, y + 6.0f, 11.0f, kRed, m->netplay_status, 230.0f);
    }
    info_plate(g, 304.0f, 248.0f, 290.0f, 150.0f, "ADDRESS",
               "Enter the address shown on the host's room screen. Left and "
               "right choose a number, up and down change it, and the shoulder "
               "buttons change it faster. The host must already have a room "
               "open. X types it with a keyboard instead.");
    hints(g, "CONNECT", "EXIT", "X:TYPE IT");

    if (in.confirm || in.start) {
        direct_join(m);
    } else if (in.alt) {
        /* The stock dialog: a keyboard, and room for a password. */
        std::snprintf(m->netplay_direct_ip, sizeof m->netplay_direct_ip,
                      "%d.%d.%d.%d", g_state.direct_ip[0], g_state.direct_ip[1],
                      g_state.direct_ip[2], g_state.direct_ip[3]);
        std::snprintf(m->netplay_direct_port, sizeof m->netplay_direct_port, "%d",
                      g_state.direct_port);
        m->netplay_password[0] = '\0';
        m->netplay_status[0] = '\0';
        m->netplay_direct_modal_open = true;
        g_state.direct = false;
    } else if (in.back) {
        g_state.direct = false;
        m->netplay_status[0] = '\0';
    }
}

/* ---- room list ------------------------------------------------------------------- */

static void draw_net_rooms(LauncherModel* m, const Gfx& g, const Input& in) {
    const auto* np = np_cb(m);
    if (!np) { launcher_model_set_view(m, LNG_VIEW_DASHBOARD); return; }
    net_setup(m);
    const bool lan = m->netplay_mode == 1;
    if (!m->netplay_list_fresh) np_refresh_lobby_list(m);
    net_tick(m, np);
    if (np->list_scope_set)
        np->list_scope_set(np->ctx, lan ? RECOMP_LAUNCHER_LIST_SCOPE_LAN
                                        : RECOMP_LAUNCHER_LIST_SCOPE_ONLINE);

    const bool online = np->connected && np->connected(np->ctx);
    const bool connecting = np->connecting && np->connecting(np->ctx);
    if (std::strncmp(m->netplay_status, "Connecting", 10) == 0) {
        if (online) m->netplay_status[0] = '\0';
        else if (!connecting && m->netplay_list_fresh)
            std::snprintf(m->netplay_status, sizeof m->netplay_status,
                          "Could not reach lobby server.");
    }
    /* Rooms come and go; ask again every couple of seconds so nobody has to
     * press a refresh button to see a friend's room appear. */
    const double now = ImGui::GetTime();
    if (now >= g_state.next_list) {
        g_state.next_list = now + 2.0;
        if (np->request_list && (lan || online)) np->request_list(np->ctx);
    }
    np_push_blocks(m);

    if (g_state.direct) { draw_net_direct(m, g, in); return; }

    std::vector<RecompLauncherCNetplayLobby> rooms;
    std::vector<int> room_index;   /* the backend's index for each */
    {
        const int n = np->list_count ? np->list_count(np->ctx) : 0;
        for (int i = 0; i < n; ++i) {
            RecompLauncherCNetplayLobby r;
            std::memset(&r, 0, sizeof r);
            if (!np->list_get || !np->list_get(np->ctx, i, &r)) continue;
            rooms.push_back(r);
            room_index.push_back(i);
        }
    }

    /* One list: HOST A ROOM, the rooms, then the fixed lines. */
    enum Extra { EX_DIRECT = 0, EX_OPTIONS, EX_CLASSIC, EX_EXIT, EX__COUNT };
    int extras[EX__COUNT];
    int ne = 0;
    if (lan) extras[ne++] = EX_DIRECT;
    extras[ne++] = EX_OPTIONS;
    extras[ne++] = EX_CLASSIC;
    extras[ne++] = EX_EXIT;
    const int nr = (int)rooms.size();
    const int nl = 1 + nr;
    const int count = nl + ne;
    const int visible = 7;
    move_cursor(PAGE_NET_ROOMS, count, in, visible);
    int& cur = g_state.cursor[PAGE_NET_ROOMS];
    const int top = g_state.top[PAGE_NET_ROOMS];
    const bool disc = launcher_model_netplay_disc_ok(m);

    bezel(g, lan ? "LAN PLAY" : "ONLINE PLAY");
    static const char* const extra_label[EX__COUNT] = {
        "JOIN BY ADDRESS", "HOST WITH OPTIONS", "ALL NETWORK OPTIONS", "EXIT" };
    int clicked = -1;
    for (int i = top; i < count && i < top + visible; ++i) {
        bool hovered = false;
        std::string label;
        char value[32] = "";
        bool enabled = disc;
        if (i == 0) {
            label = "HOST A ROOM";
        } else if (i < nl) {
            const RecompLauncherCNetplayLobby& r = rooms[i - 1];
            label = upper(r.name[0] ? r.name : "Unnamed room");
            if (label.size() > 22) label = label.substr(0, 21) + ".";
            std::snprintf(value, sizeof value, "%s%d/%d",
                          r.has_password ? "LOCK  " : "", r.player_count, r.max_slots);
        } else {
            const int ex = extras[i - nl];
            label = extra_label[ex];
            enabled = ex == EX_CLASSIC || ex == EX_EXIT || disc;
        }
        if (row(g, kListX, kListY + kRowH * (i - top), kValueR, i == cur, enabled,
                label.c_str(), value, &hovered))
            clicked = i;
        if (hover_moved(hovered)) cur = i;
    }
    scroll_marks(g, kListY, kListY + kRowH * visible - 6.0f, top > 0, count > top + visible);

    /* Bottom left: search state, this machine. */
    {
        float y = 292.0f;
        char buf[96];
        if (nr == 0) {
            text(g, kListX, y, 11.0f, kGrey,
                 !lan && !online ? (connecting ? "CONNECTING TO THE LOBBY SERVER"
                                               : "NOT CONNECTED")
                                 : std::fmod(now, 1.6) < 0.8 ? "SEARCHING FOR ROOMS"
                                                             : "SEARCHING FOR ROOMS .");
            y += 20.0f;
        }
        status_line(g, y, "NAME", kWhite, upper(np_effective_name(m)).c_str(), kListX, 280.0f);
        if (lan && address_usable(m->netplay_host_ip))
            status_line(g, y, "ADDRESS", kWhite, m->netplay_host_ip, kListX, 280.0f);
        std::snprintf(buf, sizeof buf, "%d", nr);
        status_line(g, y, "ROOMS", kWhite, buf, kListX, 280.0f);
    }

    /* The plate: what the chosen line is. */
    std::string body;
    std::string cap;
    if (cur == 0) {
        cap = "HOST";
        body = lan ? "Open a room on this network. Other players here see it "
                     "in their list within a few seconds. You are player 1."
                   : "Open a room on the lobby server for another player to "
                     "join. You are player 1.";
    } else if (cur < nl) {
        const RecompLauncherCNetplayLobby& r = rooms[cur - 1];
        char buf[160];
        cap = "ROOM";
        body = upper(r.name[0] ? r.name : "Unnamed room");
        std::snprintf(buf, sizeof buf, "\nPLAYERS %d OF %d   PASSWORD %s",
                      r.player_count, r.max_slots, r.has_password ? "YES" : "NO");
        body += buf;
        if (r.latency_ms >= 0) {
            std::snprintf(buf, sizeof buf, "   DELAY %d MS", r.latency_ms);
            body += buf;
        }
        if (std::strncmp(r.lobby_id, "lan:", 4) == 0)
            body += std::string("\nADDRESS ") + (r.lobby_id + 4);
        body += r.player_count >= r.max_slots ? "\nThis room is full."
                                              : "\nJoin as player 2.";
    } else {
        const int ex = extras[cur - nl];
        cap = ex == EX_DIRECT ? "ADDRESS" : ex == EX_OPTIONS ? "OPTIONS"
            : ex == EX_CLASSIC ? "ALL OPTIONS" : "EXIT";
        body = ex == EX_DIRECT
                   ? "Dial a host's address number by number, for a room that "
                     "does not show in the list."
             : ex == EX_OPTIONS
                   ? "Host with a room name or a password of your choosing. "
                     "Needs a keyboard."
             : ex == EX_CLASSIC
                   ? "The full network page: match settings, chat and the "
                     "rest. Easiest with a mouse."
                   : "Back to choosing LAN or online.";
    }
    if (m->netplay_status[0])
        info_plate(g, 304.0f, 288.0f, 290.0f, 110.0f, cap.c_str(), m->netplay_status, kRed);
    else if (!disc && !(cur >= nl && (extras[cur - nl] == EX_CLASSIC || extras[cur - nl] == EX_EXIT)))
        info_plate(g, 304.0f, 288.0f, 290.0f, 110.0f, cap.c_str(), disc_complaint(m), kRed);
    else
        info_plate(g, 304.0f, 288.0f, 290.0f, 110.0f, cap.c_str(), body.c_str());
    hints(g, "SELECT", "EXIT", "X:REFRESH");

    if (clicked >= 0) cur = clicked;
    if (in.confirm || clicked >= 0) {
        if (cur == 0) {
            host_room(m);
        } else if (cur < nl) {
            if (!disc) {
                std::snprintf(m->netplay_status, sizeof m->netplay_status, "%s",
                              disc_complaint(m));
            } else {
                m->netplay_selected_lobby = room_index[cur - 1];
                m->netplay_status[0] = '\0';
                np_join_selected(m);
            }
        } else {
            switch (extras[cur - nl]) {
                case EX_DIRECT:  direct_open(m); break;
                case EX_OPTIONS: host_room_dialog(m); break;
                case EX_CLASSIC: g_state.classic_net = true; break;
                default:         launcher_model_set_view(m, LNG_VIEW_NETPLAY_MODE); break;
            }
        }
    } else if (in.alt) {
        np_refresh_lobby_list(m);
        g_state.next_list = now + 2.0;
        note("SEARCHING");
    } else if (in.back) {
        launcher_model_set_view(m, LNG_VIEW_NETPLAY_MODE);
    }
}

/* ---- the room ---------------------------------------------------------------------- */

enum LobbyRow { LOBBY_START = 0, LOBBY_CARD, LOBBY_SETTINGS, LOBBY_MODS, LOBBY_LEAVE };

static void draw_lobby_page(LauncherModel* m, const LauncherTheme& th,
                            const Gfx& g, const Input& in) {
    const auto* np = np_cb(m);
    if (!np) { launcher_model_set_view(m, LNG_VIEW_DASHBOARD); return; }
    /* The stock room page's upkeep, in its order. */
    if (np->pump) np->pump(np->ctx);
    np_ingest_last_error(m, np);
    const int has_card = np_local_memcard_has_card(m);
    if (np->memcard_offer_set && has_card >= 0)
        (void)np->memcard_offer_set(np->ctx, has_card, -1);
    if (np->local_launch_gate_set)
        np->local_launch_gate_set(np->ctx, launcher_model_can_launch(m) ? 1 : 0);
    if (np->launch_pending && np->launch_pending(np->ctx))
        np_try_launch(m);
    if (!np_lobby_seated(m, np)) {
        bezel(g, "VERSUS ROOM");
        text(g, kListX, kListY, kItem, kGreen, "LEAVING THE ROOM");
        return;
    }
    /* Two seats, nothing to trade: an ask to swap is answered no. */
    if (np->seat_swap_incoming && np->seat_swap_respond) {
        char who[64] = {0};
        int from_slot = -1;
        if (np->seat_swap_incoming(np->ctx, who, sizeof who, &from_slot))
            (void)np->seat_swap_respond(np->ctx, 0);
    }

    LobbySnapshot s;
    np_lobby_snapshot(m, np, &s);

    int local_slot = -1;
    for (int i = 0; i < s.max_slots; ++i)
        if (s.occupied[i] && s.slots[i].is_local) local_slot = i;

    /* The guest's own memory card: the guest offers it, the host allows it. */
    const bool card_known = np->memcard_offer_set && has_card >= 0 &&
                            s.max_slots > 1 && s.occupied[1] &&
                            s.slots[1].memcard_offer_valid;
    const bool card_allowed = !np->guest_memcard_get || np->guest_memcard_get(np->ctx) != 0;
    const bool card_offered = card_known && s.slots[1].memcard_has_card != 0;
    const bool card_shared = card_offered && s.slots[1].memcard_share != 0;
    const bool card_row = card_known && (s.is_host ? np->guest_memcard_set != nullptr
                                                   : local_slot == 1 && card_offered);

    const bool can_start = s.seated_players >= 2 && !s.local_rom_missing &&
                           s.peers_not_ready == 0 && s.peers_missing_files == 0 &&
                           s.local_missing_files == 0;
    const char* waiting =
        s.seated_players < 2 ? "Waiting for another player to join."
        : s.local_rom_missing ? "The game disc was not found on this machine."
        : s.local_missing_files > 0 ? "This room's mods need files you have not selected."
        : s.peers_not_ready > 0 ? "Waiting for the other player to be ready."
        : s.peers_missing_files > 0 ? "The other player is missing files this room's mods need."
        : nullptr;

    struct Entry { LobbyRow id; const char* label; bool toggle; bool on; };
    Entry entries[5];
    int count = 0;
    if (s.is_host)
        entries[count++] = { LOBBY_START, "START MATCH", false, false };
    if (card_row)
        entries[count++] = { LOBBY_CARD, s.is_host ? "GUEST MEMORY CARD"
                                                   : "BRING MY MEMORY CARD",
                             true, s.is_host ? card_allowed : card_shared };
    entries[count++] = { LOBBY_SETTINGS, s.is_host ? "ROOM SETTINGS" : "ROOM INFO",
                         false, false };
    if (m->mods)
        entries[count++] = { LOBBY_MODS, s.is_host ? "ROOM MODS" : "VIEW ROOM MODS",
                             false, false };
    entries[count++] = { LOBBY_LEAVE, "LEAVE ROOM", false, false };

    move_cursor(PAGE_LOBBY, count, in, count);
    int& cur = g_state.cursor[PAGE_LOBBY];

    bezel(g, "VERSUS ROOM");

    /* Players: 1P and 2P, as the game labels its two sides. */
    const int seats = s.max_slots > 2 ? 2 : s.max_slots;
    for (int i = 0; i < seats; ++i) {
        const float y = kListY + 26.0f * i;
        char buf[96];
        std::snprintf(buf, sizeof buf, "%dP", i + 1);
        text(g, kListX, y, 15.0f, i == 0 ? kBlue : kRed, buf);
        if (!s.occupied[i]) {
            text(g, kListX + 44.0f, y, 15.0f, kGreenDim, "- OPEN -");
            continue;
        }
        const RecompLauncherCNetplayMember& mem = s.slots[i];
        std::string name = upper(mem.display_name);
        if (name.size() > 20) name = name.substr(0, 19) + ".";
        if (mem.is_local) name += "  (YOU)";
        text(g, kListX + 44.0f, y, 15.0f, kGreen, name.c_str());
        const char* state;
        if (mem.is_host) {
            state = "HOST";
        } else {
            const bool ready = mem.ready &&
                               !(mem.mod_readiness_valid && mem.mods_missing > 0);
            state = ready ? "READY" : "WAIT";
            if (mem.latency_ms >= 0 && !mem.is_local) {
                std::snprintf(buf, sizeof buf, "%d MS", mem.latency_ms);
                text_right(g, kValueR - 80.0f, y + 3.0f, 11.0f, kGrey, buf);
            }
        }
        text_right(g, kValueR, y, 15.0f, kWhite, state);
    }
    g.dl->AddLine(at(g, kListX, kListY + 58.0f), at(g, kValueR, kListY + 58.0f),
                  IM_COL32(60, 60, 58, 255));

    const float cmd_y = kListY + 70.0f;
    int clicked = -1;
    for (int i = 0; i < count; ++i) {
        bool hovered = false;
        const bool enabled = entries[i].id != LOBBY_START || can_start;
        if (row(g, kListX, cmd_y + kRowH * i, 290.0f, i == cur, enabled,
                entries[i].label,
                entries[i].toggle ? (entries[i].on ? "ON" : "OFF") : nullptr,
                &hovered))
            clicked = i;
        /* A greyed START still takes the cursor, so its reason can be read. */
        if (hover_moved(hovered)) cur = i;
    }

    /* Right: the room. */
    {
        float y = cmd_y + 4.0f;
        char buf[128];
        const float x0 = 330.0f;
        status_line(g, y, "MATCH", kWhite, "VERSUS, FULL SCREEN", x0);
        std::snprintf(buf, sizeof buf, "%d OF %d", s.seated_players, s.max_slots);
        status_line(g, y, "PLAYERS", kWhite, buf, x0);
        if (m->netplay_local_room && m->netplay_host_endpoint[0])
            status_line(g, y, "ADDRESS", kWhite, m->netplay_host_endpoint, x0);
        if (card_known)
            status_line(g, y, "2P CARD", kWhite,
                        card_shared && card_allowed ? "GUEST'S OWN" : "HOST'S SLOT 2", x0);
    }
    std::string body;
    const char* cap;
    ImU32 body_color = kWhite;
    if (m->netplay_status[0]) {
        cap = "NOTICE"; body = m->netplay_status; body_color = kRed;
    } else if (waiting) {
        cap = "STANDBY"; body = waiting;
        if (s.is_host && s.seated_players < 2 && m->netplay_local_room)
            body += " Players on this network see the room in their list. "
                    "Anyone else can join by the address above.";
    } else if (s.is_host) {
        cap = "READY"; body = "Both players are in. Start the match when you are ready.";
    } else {
        cap = "READY"; body = "Waiting for the host to start the match.";
    }
    info_plate(g, 304.0f, 288.0f, 290.0f, 110.0f, cap, body.c_str(), body_color);
    hints(g, "SELECT", "LEAVE", s.is_host ? "START:START MATCH" : nullptr);

    tg_menu_lobby_popups(m, th, np, s);

    int activate = -1;
    if (clicked >= 0) activate = cur = clicked;
    else if (in.confirm) activate = cur;
    else if (in.start && s.is_host) activate = 0;
    else if (in.back && entries[cur].id != LOBBY_LEAVE) {
        /* Back does not drop a seat by accident: it goes to LEAVE ROOM, and
         * a second press there leaves. */
        cur = count - 1;
        note("PRESS AGAIN TO LEAVE THE ROOM");
    } else if (in.back) {
        activate = cur;
    }
    if (activate < 0) return;
    switch (entries[activate].id) {
        case LOBBY_START:
            if (can_start) np_lobby_start(m, np);
            else note("NOT READY TO START YET");
            break;
        case LOBBY_CARD:
            if (s.is_host) {
                if (np->guest_memcard_set)
                    (void)np->guest_memcard_set(np->ctx, card_allowed ? 0 : 1);
            } else {
                (void)np->memcard_offer_set(np->ctx, has_card > 0 ? 1 : 0,
                                            card_shared ? 0 : 1);
            }
            break;
        case LOBBY_SETTINGS:
            g_lobby_settings_synced = false;
            m->netplay_lobby_settings_open = true;
            break;
        case LOBBY_MODS:
            m->netplay_lobby_mods_open = true;
            break;
        case LOBBY_LEAVE:
            g_state.cursor[PAGE_LOBBY] = 0;
            np_lobby_leave(m, np);
            break;
    }
}
