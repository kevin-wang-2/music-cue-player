"""
api_test — comprehensive API test for the mcp scripting layer.

Designed to be run from the "Run API Test" scriptlet cue in api_test.mcp.
The test creates/deletes a temporary cue list — your existing show is not modified.
"""


def run():
    import mcp
    import mcp.cue
    import mcp.cue_list
    import mcp.event
    import mcp.time
    import mcp.error

    # ── result collector ──────────────────────────────────────────────────
    results = []

    def ok(label):
        results.append(f"  ✓  {label}")

    def fail(label, exc=None):
        suffix = f": {exc}" if exc else ""
        results.append(f"  ✗  {label}{suffix}")

    def section(title):
        results.append(f"\n── {title} ──")

    # ══════════════════════════════════════════════════════════════════════
    # 1. Dialogs
    # ══════════════════════════════════════════════════════════════════════
    section("Dialogs")

    try:
        mcp.alert("api_test: alert() — click OK.")
        ok("alert()")
    except Exception as e:
        fail("alert()", e)

    try:
        r = mcp.confirm("api_test: confirm() — click YES.")
        ok(f"confirm() → {r!r}") if r is True else fail("confirm() expected True")
    except Exception as e:
        fail("confirm()", e)

    try:
        path = mcp.file(title="api_test: pick any file (or cancel)")
        ok(f"file(mode='open') → {path!r}")
    except Exception as e:
        fail("file(mode='open')", e)

    try:
        d = mcp.file(title="api_test: pick any folder (or cancel)", mode="dir")
        ok(f"file(mode='dir') → {d!r}")
    except Exception as e:
        fail("file(mode='dir')", e)

    try:
        text = mcp.input("Type anything (or cancel):", default="hello", title="api_test: input()")
        ok(f"input() → {text!r}")
    except Exception as e:
        fail("input()", e)

    # ══════════════════════════════════════════════════════════════════════
    # 2. get_state / get_mc
    # ══════════════════════════════════════════════════════════════════════
    section("State & MC")

    try:
        state = mcp.get_state()
        assert isinstance(state, dict)
        for k in ("selected_cue", "running_cues", "mc_master"):
            assert k in state, f"missing key '{k}'"
        assert isinstance(state["running_cues"], list)
        sel = state["selected_cue"]
        ok(f"get_state() → selected={sel!r}  running={len(state['running_cues'])} cue(s)")
    except Exception as e:
        fail("get_state()", e)

    try:
        mc = mcp.get_mc()
        assert isinstance(mc, dict)
        for k in ("bpm", "time_signature", "bar", "beat", "PPQ"):
            assert k in mc, f"missing '{k}'"
        ok(f"get_mc() → bpm={mc['bpm']}  ts={mc['time_signature']}  "
           f"bar={mc['bar']}  beat={mc['beat']}")
    except Exception as e:
        fail("get_mc()", e)

    # ══════════════════════════════════════════════════════════════════════
    # 3. mcp.cue module
    # ══════════════════════════════════════════════════════════════════════
    section("mcp.cue module functions")

    try:
        sel = mcp.cue.get_selected_cue()
        ok(f"get_selected_cue() → {sel!r}")
    except Exception as e:
        fail("get_selected_cue()", e)

    try:
        cur = mcp.cue.get_current_cue()
        ok(f"get_current_cue() → {cur!r}")
    except Exception as e:
        fail("get_current_cue()", e)

    try:
        active = mcp.cue.get_active_cues()
        assert isinstance(active, list)
        ok(f"get_active_cues() → {len(active)} active")
    except Exception as e:
        fail("get_active_cues()", e)

    # ══════════════════════════════════════════════════════════════════════
    # 4. Cue object properties (uses cue "1" — Audio Cue)
    # ══════════════════════════════════════════════════════════════════════
    section("Cue object properties")

    cl_main = mcp.cue_list.get_active_cue_list()
    all_cues = cl_main.list_cues()

    if not all_cues:
        fail("No cues in active list — skipped")
    else:
        c = all_cues[0]
        try:
            ok(f"cue[0]: index={c.index}  number={c.number!r}  "
               f"name={c.name!r}  type={c.type!r}")
            ok(f"  pre_wait={c.pre_wait}  auto_continue={c.auto_continue}  "
               f"auto_follow={c.auto_follow}")
            ok(f"  is_playing={c.is_playing}  is_pending={c.is_pending}  "
               f"is_armed={c.is_armed}")
        except Exception as e:
            fail("Cue basic properties", e)

        try:
            old = c.name
            c.name = old + "_x"
            assert c.name == old + "_x"
            c.name = old
            ok("cue.name setter (round-trip)")
        except Exception as e:
            fail("cue.name setter", e)

        audio = next((x for x in all_cues if x.type == "audio"), None)
        if audio:
            try:
                ok(f"AudioCue: path={audio.path!r}")
                ok(f"  level={audio.level}  trim={audio.trim}  "
                   f"duration={audio.duration:.3f}s  start_time={audio.start_time}")
            except Exception as e:
                fail("AudioCue properties", e)
        else:
            ok("audio properties — no audio cue found (skipped)")

        try:
            c.select()
            ok("cue.select()")
        except Exception as e:
            fail("cue.select()", e)

    # ══════════════════════════════════════════════════════════════════════
    # 5. mcp.cue_list — list management
    # ══════════════════════════════════════════════════════════════════════
    section("mcp.cue_list — list management")

    try:
        lists = mcp.cue_list.list_cue_lists()
        assert isinstance(lists, list) and len(lists) >= 1
        ok(f"list_cue_lists() → {len(lists)} list(s)  ids={[l.id for l in lists]}")
    except Exception as e:
        fail("list_cue_lists()", e)

    try:
        a = mcp.cue_list.get_active_cue_list()
        ok(f"get_active_cue_list() → id={a.id}")
    except Exception as e:
        fail("get_active_cue_list()", e)

    test_cl = test_cl2 = None
    try:
        test_cl = mcp.cue_list.insert_cue_list("_API_TEST_")
        ok(f"insert_cue_list() → id={test_cl.id}")
    except Exception as e:
        fail("insert_cue_list()", e)

    if test_cl:
        try:
            test_cl2 = mcp.cue_list.insert_cue_list_at(test_cl, "_API_TEST_2_")
            ok(f"insert_cue_list_at() → id={test_cl2.id}")
        except Exception as e:
            fail("insert_cue_list_at()", e)

    # ══════════════════════════════════════════════════════════════════════
    # 6. CueList methods (including new cue types)
    # ══════════════════════════════════════════════════════════════════════
    section("CueList methods (temporary list)")

    if test_cl:
        try:
            assert test_cl.list_cues() == []
            ok("list_cues() on empty → []")
        except Exception as e:
            fail("list_cues() empty", e)

        cue_a = cue_b = cue_g = None

        try:
            cue_a = test_cl.insert_cue("memo", cuenumber="T1", cuename="Test A")
            ok(f"insert_cue('memo', cuenumber='T1', ...) → {cue_a!r}")
        except Exception as e:
            fail("insert_cue()", e)

        try:
            cue_b = test_cl.insert_cue("memo", cuenumber="T2", cuename="Test B")
            ok(f"insert_cue() second → {cue_b!r}")
        except Exception as e:
            fail("insert_cue() second", e)

        # New cue types: deactivate and reactivate
        try:
            cue_dact = test_cl.insert_cue("deactivate", cuenumber="T3", cuename="Deactivate Plugin")
            assert cue_dact.type == "deactivate"
            ok(f"insert_cue('deactivate') → type={cue_dact.type!r}")
        except Exception as e:
            fail("insert_cue('deactivate')", e)

        try:
            cue_react = test_cl.insert_cue("reactivate", cuenumber="T4", cuename="Reactivate Plugin")
            assert cue_react.type == "reactivate"
            ok(f"insert_cue('reactivate') → type={cue_react.type!r}")
        except Exception as e:
            fail("insert_cue('reactivate')", e)

        try:
            if cue_a:
                cue_g = test_cl.insert_cue_at(
                    cue_a, "group", cuenumber="T1.5", cuename="Test Group")
                ok(f"insert_cue_at(after T1) → {cue_g!r}")
                fresh = test_cl.list_cues()
                if len(fresh) >= 3:
                    cue_b = fresh[2]
        except Exception as e:
            fail("insert_cue_at()", e)

        try:
            nums = [c.number for c in test_cl.list_cues()]
            ok(f"list_cues() after inserts → {nums}")
        except Exception as e:
            fail("list_cues() after inserts", e)

        try:
            if cue_a and cue_b:
                test_cl.move_cue_at(cue_a, cue_b)
                ok(f"move_cue_at(ref=T1, cue=T2) → {[c.number for c in test_cl.list_cues()]}")
        except Exception as e:
            fail("move_cue_at()", e)

        try:
            fresh = test_cl.list_cues()
            if len(fresh) >= 3 and cue_g:
                ref_fresh = fresh[1]
                cue_fresh = fresh[2]
                test_cl.move_cue_at(ref_fresh, cue_fresh, to_group=True)
                ok(f"move_cue_at(to_group=True) → {[c.number for c in test_cl.list_cues()]}")
        except Exception as e:
            fail("move_cue_at(to_group=True)", e)

        try:
            if cue_a:
                test_cl.delete_cue(cue_a)
                ok("delete_cue(cue_a)")
        except Exception as e:
            fail("delete_cue()", e)

        try:
            while True:
                remaining = test_cl.list_cues()
                if not remaining:
                    break
                test_cl.delete_cue(remaining[0])
            ok("delete remaining → []")
        except Exception as e:
            fail("delete remaining", e)

    try:
        if test_cl2:
            mcp.cue_list.delete_cue_list(test_cl2)
            ok("delete_cue_list(_API_TEST_2_)")
    except Exception as e:
        fail("delete_cue_list(2)", e)

    try:
        if test_cl:
            mcp.cue_list.delete_cue_list(test_cl)
            ok("delete_cue_list(_API_TEST_)")
    except Exception as e:
        fail("delete_cue_list(1)", e)

    # ══════════════════════════════════════════════════════════════════════
    # 7. mcp.time
    # ══════════════════════════════════════════════════════════════════════
    section("mcp.time")

    from mcp.time import Time

    try:
        t = Time.from_min_sec(1, 30)
        assert abs(t.to_seconds() - 90.0) < 0.001
        min_s = t.to_min_sec()
        ok(f"from_min_sec(1,30) → {t.to_seconds()}s  to_min_sec={min_s}")
    except Exception as e:
        fail("from_min_sec / to_min_sec", e)

    try:
        t = Time.from_sample(44100)
        ok(f"from_sample(44100) → {t.to_seconds():.4f}s  "
           f"to_samples={t.to_samples()}")
    except Exception as e:
        fail("from_sample / to_samples", e)

    try:
        t = Time.from_timecode("01:00:00:00", fps=25)
        assert abs(t.to_seconds() - 3600.0) < 0.01
        ok(f"from_timecode('01:00:00:00', fps=25) → {t.to_seconds()}s")
        ok(f"  to_timecode(fps=25) → {t.to_timecode(fps=25)}")
    except Exception as e:
        fail("from_timecode / to_timecode", e)

    try:
        state = mcp.get_state()
        mc_cue = state.get("mc_master")
        if mc_cue is not None:
            t = Time.from_bar_beat(2, 1, mc_cue)
            ok(f"from_bar_beat(2,1,mc_cue) → {t.to_seconds():.3f}s")
        else:
            ok("from_bar_beat — skipped (start the MC Group cue first)")
    except Exception as e:
        ok(f"from_bar_beat raised {type(e).__name__} (expected without MC): {e}")

    # ══════════════════════════════════════════════════════════════════════
    # 8. mcp.event — registration
    # ══════════════════════════════════════════════════════════════════════
    section("mcp.event — registration")

    try:
        log = []
        mcp.event.on_cue_fired    (lambda c: log.append(f"fired:{c.number}"))
        mcp.event.on_cue_selected (lambda c: log.append(f"selected:{c.number}"))
        mcp.event.on_cue_inserted (lambda c: log.append(f"inserted:{c.number}"))
        mcp.event.on_osc_event    (lambda p: log.append(f"osc:{p}"))
        mcp.event.on_midi_event   (lambda t, ch, d1, d2: log.append(f"midi:{t},{ch},{d1},{d2}"))
        mcp.event.on_music_event  ("1/4", lambda: log.append("beat"))
        mcp.event.once_cue_fired  (lambda c: log.append(f"once_fired:{c.number}"))
        ok("on_cue_fired / on_cue_selected / on_cue_inserted / "
           "on_osc_event / on_midi_event / on_music_event / once_cue_fired registered")
    except Exception as e:
        fail("event registration", e)

    try:
        mcp.event.clear_all()
        ok("clear_all()")
    except Exception as e:
        fail("clear_all()", e)

    # ══════════════════════════════════════════════════════════════════════
    # 9. mcp.error
    # ══════════════════════════════════════════════════════════════════════
    section("mcp.error — exception types")

    for name in ("CueNotFoundError", "CueTypeError",
                 "NoMasterContextError", "InvalidOperationError"):
        try:
            cls = getattr(mcp.error, name)
            assert issubclass(cls, Exception)
            ok(f"mcp.error.{name}")
        except Exception as e:
            fail(f"mcp.error.{name}", e)

    # ══════════════════════════════════════════════════════════════════════
    # 10. mcp.select
    # ══════════════════════════════════════════════════════════════════════
    section("mcp.select")

    try:
        first = mcp.cue_list.get_active_cue_list().list_cues()
        if first:
            orig = mcp.cue.get_selected_cue()
            mcp.select(first[0].number)
            ok(f"select('{first[0].number}') — no exception")
            if orig:
                mcp.select(orig.number)
        else:
            ok("select() — skipped (no cues)")
    except Exception as e:
        fail("mcp.select()", e)

    ok("go() — skipped (would fire the selected cue)")

    # ══════════════════════════════════════════════════════════════════════
    # 11. mcp.mix_console
    # ══════════════════════════════════════════════════════════════════════
    section("mcp.mix_console — module import & channel list")

    import mcp.mix_console as mc

    try:
        channels = mc.list_channels()
        assert isinstance(channels, list)
        ok(f"list_channels() → {len(channels)} channel(s)")
    except Exception as e:
        fail("list_channels()", e)

    # Append a test channel so we always have one to probe, regardless of show config.
    test_ch = None
    pre_count = 0
    try:
        pre_count = len(mc.list_channels())
        test_ch = mc.append_channel()
        post_count = len(mc.list_channels())
        assert post_count == pre_count + 1
        ok(f"append_channel() → {test_ch!r}  total={post_count}")
    except Exception as e:
        fail("append_channel()", e)

    section("mcp.mix_console — Channel properties")

    if test_ch is not None:
        try:
            ch0 = mc.get_channel(test_ch.ch)
            ok(f"get_channel({test_ch.ch}) → {ch0!r}")
        except Exception as e:
            fail(f"get_channel({test_ch.ch})", e)

        try:
            old_name = test_ch.name
            test_ch.name = "_api_test_ch_"
            assert test_ch.name == "_api_test_ch_"
            test_ch.name = old_name
            ok("channel.name round-trip")
        except Exception as e:
            fail("channel.name", e)

        try:
            old_fader = test_ch.fader
            test_ch.fader = -18.0
            assert abs(test_ch.fader - -18.0) < 0.5
            test_ch.fader = old_fader
            ok(f"channel.fader round-trip (was {old_fader:.1f} dB)")
        except Exception as e:
            fail("channel.fader", e)

        try:
            old_mute = test_ch.mute
            test_ch.mute = True
            assert test_ch.mute is True
            test_ch.mute = False
            assert test_ch.mute is False
            test_ch.mute = old_mute
            ok("channel.mute round-trip")
        except Exception as e:
            fail("channel.mute", e)

        try:
            old_pol = test_ch.polarity
            test_ch.polarity = True
            assert test_ch.polarity is True
            test_ch.polarity = old_pol
            ok("channel.polarity round-trip")
        except Exception as e:
            fail("channel.polarity", e)

        try:
            old_delay = test_ch.delay
            test_ch.delay = 5.0
            assert abs(test_ch.delay - 5.0) < 0.1
            test_ch.delay = old_delay
            ok(f"channel.delay round-trip (was {old_delay} ms)")
        except Exception as e:
            fail("channel.delay", e)

        try:
            old_pdc = test_ch.pdc_isolation
            test_ch.pdc_isolation = True
            assert test_ch.pdc_isolation is True
            test_ch.pdc_isolation = old_pdc
            ok("channel.pdc_isolation round-trip")
        except Exception as e:
            fail("channel.pdc_isolation", e)

        try:
            ls = test_ch.get_link_state()
            assert isinstance(ls, str)
            ok(f"channel.get_link_state() → {ls!r}")
        except Exception as e:
            fail("channel.get_link_state()", e)

        try:
            old_xp = test_ch.get_crosspoint(0)
            test_ch.set_crosspoint(0, -60.0)
            v = test_ch.get_crosspoint(0)
            assert abs(v - -60.0) < 1.0
            test_ch.set_crosspoint(0, old_xp)
            ok("channel.get_crosspoint / set_crosspoint round-trip")
        except Exception as e:
            fail("channel.get_crosspoint / set_crosspoint", e)

    section("mcp.mix_console — PluginSlot & SendSlot")

    if test_ch is not None:
        try:
            ps = test_ch.get_plugin_slot(0)
            ok(f"channel.get_plugin_slot(0) → {ps!r}")
            params = ps.list_params()
            assert isinstance(params, list)
            ok(f"plugin_slot.list_params() → {len(params)} param(s)  "
               f"(0 expected — no plugin loaded)")
        except Exception as e:
            fail("channel.get_plugin_slot / list_params", e)

        try:
            ss = test_ch.get_send_slot(0)
            ok(f"channel.get_send_slot(0) → {ss!r}")
            ok(f"  send_slot.level={ss.level}  mute={ss.mute}  pan={ss.pan}")
        except Exception as e:
            fail("channel.get_send_slot", e)

        try:
            ss = test_ch.get_send_slot(0)
            old_lv = ss.level
            ss.level = -6.0
            assert abs(ss.level - -6.0) < 0.5
            ss.level = old_lv
            ok("send_slot.level round-trip")
        except Exception as e:
            fail("send_slot.level", e)

        try:
            ss = test_ch.get_send_slot(0)
            old_mu = ss.mute
            ss.mute = True
            assert ss.mute is True
            ss.mute = old_mu
            ok("send_slot.mute round-trip")
        except Exception as e:
            fail("send_slot.mute", e)

    section("mcp.mix_console — get_param / set_param")

    if pre_count > 0 or test_ch is not None:
        target_ch = 0
        path = f"/mixer/{target_ch}/fader"
        try:
            old_v = mc.get_param(path)
            mc.set_param(path, -24.0)
            v = mc.get_param(path)
            assert abs(v - -24.0) < 0.5
            mc.set_param(path, old_v)
            ok(f"get_param / set_param round-trip → '{path}'")
        except Exception as e:
            fail(f"get_param / set_param ('{path}')", e)

        try:
            ok(f"get_param('/mixer/{target_ch}/mute') → "
               f"{mc.get_param('/mixer/' + str(target_ch) + '/mute')}")
        except Exception as e:
            fail("get_param mute", e)
    else:
        ok("get_param / set_param — skipped (no channels)")

    section("mcp.mix_console — snapshots")

    try:
        snaps = mc.list_snapshot()
        assert isinstance(snaps, list)
        ok(f"list_snapshot() → {len(snaps)} snapshot(s)")
        for s in snaps:
            assert "id" in s and "name" in s
        if snaps:
            ok(f"  first: id={snaps[0]['id']}  name={snaps[0]['name']!r}")
    except Exception as e:
        fail("list_snapshot()", e)

    # scope helpers (no snapshot needed for basic API check)
    try:
        snaps = mc.list_snapshot()
        if snaps:
            sid = snaps[0]["id"]
            mc.set_snapshot_scope(sid, "/mixer/0/fader")
            assert mc.check_snapshot_scope(sid, "/mixer/0/fader") is True
            mc.unset_snapshot_scope(sid, "/mixer/0/fader")
            assert mc.check_snapshot_scope(sid, "/mixer/0/fader") is False
            ok(f"set/unset/check_snapshot_scope on id={sid}")
        else:
            ok("snapshot scope API — skipped (no snapshots)")
    except Exception as e:
        fail("snapshot scope API", e)

    section("mcp.mix_console — cleanup")

    try:
        if test_ch is not None:
            mc.remove_channel(test_ch)
            after = len(mc.list_channels())
            assert after == pre_count
            ok(f"remove_channel() — back to {after} channel(s)")
    except Exception as e:
        fail("remove_channel()", e)

    # ══════════════════════════════════════════════════════════════════════
    # Report
    # ══════════════════════════════════════════════════════════════════════
    total  = sum(1 for r in results if "✓" in r or "✗" in r)
    passed = sum(1 for r in results if "✓" in r)
    failed = sum(1 for r in results if "✗" in r)

    summary = f"api_test: {passed}/{total} passed"
    if failed:
        summary += f"  ({failed} FAILED)"

    report = summary + "\n" + "\n".join(results)
    print(report)
    mcp.alert(summary)
