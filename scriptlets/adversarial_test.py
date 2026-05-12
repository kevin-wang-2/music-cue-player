"""
adversarial_test — throws illegal inputs at every API surface.

Scoring:
  ✓  exception raised  (program handled gracefully)
  ⚠  no exception     (bad input silently ignored — candidate for a future fix)

Run via scriptlet code:
    import mcp.library.adversarial_test as t; t.run()
"""


def run():
    import mcp
    import mcp.cue
    import mcp.cue_list
    import mcp.error
    import mcp.event
    import mcp.time
    import mcp.mix_console as mc
    from mcp.time import Time

    results = []

    def expect_exc(label, fn):
        try:
            fn()
            results.append(f"  ⚠  {label}: no exception raised")
        except SystemExit:
            raise
        except Exception as e:
            results.append(f"  ✓  {label}: {type(e).__name__}: {e}")

    def note(text):
        results.append(f"  .  {text}")

    def section(title):
        results.append(f"\n── {title} ──")

    # ══════════════════════════════════════════════════════════════════════
    # Setup — create a temporary list with two cues, then delete one
    # ══════════════════════════════════════════════════════════════════════
    section("Setup")
    cl = mcp.cue_list.insert_cue_list("_ADV_TEST_")
    note(f"test list id={cl.id}")
    cue_a = cl.insert_cue("memo", cuenumber="A1", cuename="Adv A")
    cue_b = cl.insert_cue("memo", cuenumber="A2", cuename="Adv B")
    note(f"cue_a.index={cue_a.index}  cue_b.index={cue_b.index}")
    cl.delete_cue(cue_a)          # cue_a.index is now -1 (stale)
    note(f"deleted cue_a → index={cue_a.index}")

    # ══════════════════════════════════════════════════════════════════════
    # 1. Stale Cue object (index == -1 after delete)
    # ══════════════════════════════════════════════════════════════════════
    section("Stale Cue (index=-1 after delete)")

    expect_exc("delete_cue(stale cue)",
               lambda: cl.delete_cue(cue_a))
    expect_exc("insert_cue_at(stale ref, 'memo')",
               lambda: cl.insert_cue_at(cue_a, "memo"))
    expect_exc("move_cue_at(ref=stale, cue=live)",
               lambda: cl.move_cue_at(cue_a, cue_b))
    expect_exc("move_cue_at(ref=live, cue=stale)",
               lambda: cl.move_cue_at(cue_b, cue_a))
    expect_exc("cue.select() on stale",
               lambda: cue_a.select())
    expect_exc("cue.start() on stale",
               lambda: cue_a.start())
    expect_exc("cue.stop() on stale",
               lambda: cue_a.stop())
    expect_exc("cue.arm() on stale",
               lambda: cue_a.arm())
    expect_exc("cue.disarm() on stale",
               lambda: cue_a.disarm())
    expect_exc("cue.go() on stale",
               lambda: cue_a.go())
    expect_exc("cue.name = x on stale",
               lambda: setattr(cue_a, "name", "new"))

    # ══════════════════════════════════════════════════════════════════════
    # 2. Self-move
    # ══════════════════════════════════════════════════════════════════════
    section("Self-move")

    expect_exc("move_cue_at(cue_b, cue_b)",
               lambda: cl.move_cue_at(cue_b, cue_b))

    # ══════════════════════════════════════════════════════════════════════
    # 3. Wrong Python types — CueList methods
    # ══════════════════════════════════════════════════════════════════════
    section("Wrong Python types — CueList methods")

    expect_exc("delete_cue(None)",
               lambda: cl.delete_cue(None))
    expect_exc("delete_cue(42)",
               lambda: cl.delete_cue(42))
    expect_exc("delete_cue('A2')",
               lambda: cl.delete_cue("A2"))
    expect_exc("insert_cue_at(None, 'memo')",
               lambda: cl.insert_cue_at(None, "memo"))
    expect_exc("insert_cue_at(42, 'memo')",
               lambda: cl.insert_cue_at(42, "memo"))
    expect_exc("insert_cue_at(cue_b, None)",
               lambda: cl.insert_cue_at(cue_b, None))
    expect_exc("insert_cue_at(cue_b, 42)",
               lambda: cl.insert_cue_at(cue_b, 42))
    expect_exc("move_cue_at(None, cue_b)",
               lambda: cl.move_cue_at(None, cue_b))
    expect_exc("move_cue_at(cue_b, None)",
               lambda: cl.move_cue_at(cue_b, None))
    expect_exc("move_cue_at(cue_b, 42)",
               lambda: cl.move_cue_at(cue_b, 42))
    expect_exc("move_cue_at('x', cue_b)",
               lambda: cl.move_cue_at("x", cue_b))

    # ══════════════════════════════════════════════════════════════════════
    # 4. Wrong types — Cue property setters
    # ══════════════════════════════════════════════════════════════════════
    section("Wrong types — Cue property setters")

    expect_exc("cue.name = None",
               lambda: setattr(cue_b, "name", None))
    expect_exc("cue.name = 42",
               lambda: setattr(cue_b, "name", 42))
    expect_exc("cue.name = []",
               lambda: setattr(cue_b, "name", []))

    # ══════════════════════════════════════════════════════════════════════
    # 5. Invalid cue type strings
    # ══════════════════════════════════════════════════════════════════════
    section("Invalid cue type strings")

    expect_exc("insert_cue('nonexistent_type')",
               lambda: cl.insert_cue("nonexistent_type"))
    expect_exc("insert_cue('')",
               lambda: cl.insert_cue(""))
    expect_exc("insert_cue(None)",
               lambda: cl.insert_cue(None))
    expect_exc("insert_cue(42)",
               lambda: cl.insert_cue(42))

    # ══════════════════════════════════════════════════════════════════════
    # 6. Wrong types — top-level mcp functions (no dialog shown for bad types)
    # ══════════════════════════════════════════════════════════════════════
    section("Wrong types — mcp top-level (no-dialog cases)")

    expect_exc("alert(None)",
               lambda: mcp.alert(None))
    expect_exc("alert(42)",
               lambda: mcp.alert(42))
    expect_exc("confirm(None)",
               lambda: mcp.confirm(None))
    expect_exc("confirm([])",
               lambda: mcp.confirm([]))
    expect_exc("select(None)",
               lambda: mcp.select(None))
    expect_exc("select(42)",
               lambda: mcp.select(42))
    expect_exc("select([])",
               lambda: mcp.select([]))

    # ══════════════════════════════════════════════════════════════════════
    # 7. mcp.time bad inputs
    # ══════════════════════════════════════════════════════════════════════
    section("mcp.time bad inputs")

    expect_exc("Time.from_min_sec(None, 0)",
               lambda: Time.from_min_sec(None, 0))
    expect_exc("Time.from_min_sec('x', 0)",
               lambda: Time.from_min_sec("x", 0))
    expect_exc("Time.from_min_sec(1, None)",
               lambda: Time.from_min_sec(1, None))
    expect_exc("Time.from_sample(None)",
               lambda: Time.from_sample(None))
    expect_exc("Time.from_sample('x')",
               lambda: Time.from_sample("x"))
    expect_exc("Time.from_timecode(None, fps=25)",
               lambda: Time.from_timecode(None, fps=25))
    expect_exc("Time.from_timecode('not:valid', fps=25)",
               lambda: Time.from_timecode("not:valid", fps=25))
    expect_exc("Time.from_timecode('01:00:00:00', fps=0)",
               lambda: Time.from_timecode("01:00:00:00", fps=0))
    expect_exc("Time.from_timecode('01:00:00:00', fps=-5)",
               lambda: Time.from_timecode("01:00:00:00", fps=-5))
    expect_exc("Time.from_bar_beat(1, 1, None)",
               lambda: Time.from_bar_beat(1, 1, None))
    expect_exc("Time.from_bar_beat(1, 1, 'x')",
               lambda: Time.from_bar_beat(1, 1, "x"))
    expect_exc("Time.from_bar_beat(1, 1, 42)",
               lambda: Time.from_bar_beat(1, 1, 42))

    # ══════════════════════════════════════════════════════════════════════
    # 8. mcp.event — non-callable and invalid quantization
    # ══════════════════════════════════════════════════════════════════════
    section("mcp.event bad inputs")

    expect_exc("on_cue_fired(None)",
               lambda: mcp.event.on_cue_fired(None))
    expect_exc("on_cue_fired('string')",
               lambda: mcp.event.on_cue_fired("not a callable"))
    expect_exc("on_cue_fired(42)",
               lambda: mcp.event.on_cue_fired(42))
    expect_exc("on_cue_selected(None)",
               lambda: mcp.event.on_cue_selected(None))
    expect_exc("on_cue_inserted(None)",
               lambda: mcp.event.on_cue_inserted(None))
    expect_exc("on_osc_event(None)",
               lambda: mcp.event.on_osc_event(None))
    expect_exc("on_midi_event(None)",
               lambda: mcp.event.on_midi_event(None))
    expect_exc("on_music_event('1/3', fn)  ← bad quantization",
               lambda: mcp.event.on_music_event("1/3", lambda: None))
    expect_exc("on_music_event('bad', fn) ← bad quantization",
               lambda: mcp.event.on_music_event("bad", lambda: None))
    expect_exc("on_music_event('1/4', None)",
               lambda: mcp.event.on_music_event("1/4", None))
    expect_exc("once_cue_fired(None)",
               lambda: mcp.event.once_cue_fired(None))
    mcp.event.clear_all()

    # ══════════════════════════════════════════════════════════════════════
    # 9. mcp.select with nonexistent cue number (valid string, bad value)
    # ══════════════════════════════════════════════════════════════════════
    section("mcp.select nonexistent cue number")

    expect_exc("select('DOES_NOT_EXIST')",
               lambda: mcp.select("DOES_NOT_EXIST"))

    # ══════════════════════════════════════════════════════════════════════
    # 10. Stale CueList operations (after delete_cue_list)
    # ══════════════════════════════════════════════════════════════════════
    section("Stale CueList (after delete_cue_list)")

    while True:
        remaining = cl.list_cues()
        if not remaining:
            break
        cl.delete_cue(remaining[0])
    mcp.cue_list.delete_cue_list(cl)
    note("test list deleted")

    expect_exc("delete_cue_list(stale list) — double-delete",
               lambda: mcp.cue_list.delete_cue_list(cl))
    expect_exc("cl.list_cues() on deleted list",
               lambda: cl.list_cues())
    expect_exc("cl.insert_cue() on deleted list",
               lambda: cl.insert_cue("memo"))
    expect_exc("cl.delete_cue(cue_b) on deleted list",
               lambda: cl.delete_cue(cue_b))
    expect_exc("cl.insert_cue_at(cue_b, 'memo') on deleted list",
               lambda: cl.insert_cue_at(cue_b, "memo"))

    # ══════════════════════════════════════════════════════════════════════
    # 11. mcp.mix_console — bad inputs to module-level functions
    # ══════════════════════════════════════════════════════════════════════
    section("mcp.mix_console — module-level bad inputs")

    expect_exc("get_channel(None)",
               lambda: mc.get_channel(None))
    expect_exc("get_channel('x')",
               lambda: mc.get_channel("x"))
    expect_exc("get_channel([])",
               lambda: mc.get_channel([]))

    expect_exc("remove_channel(None)",
               lambda: mc.remove_channel(None))
    expect_exc("remove_channel('x')",
               lambda: mc.remove_channel("x"))
    expect_exc("remove_channel([])",
               lambda: mc.remove_channel([]))

    expect_exc("get_param(None)",
               lambda: mc.get_param(None))
    expect_exc("get_param(42)",
               lambda: mc.get_param(42))
    expect_exc("set_param(None, 0.0)",
               lambda: mc.set_param(None, 0.0))
    expect_exc("set_param('/mixer/0/fader', 'not-a-float')",
               lambda: mc.set_param("/mixer/0/fader", "not-a-float"))
    expect_exc("set_param('/mixer/0/fader', None)",
               lambda: mc.set_param("/mixer/0/fader", None))

    expect_exc("load_snapshot(None)",
               lambda: mc.load_snapshot(None))
    expect_exc("load_snapshot('x')",
               lambda: mc.load_snapshot("x"))
    expect_exc("load_snapshot(-9999)  ← nonexistent",
               lambda: mc.load_snapshot(-9999))
    expect_exc("save_snapshot(None)",
               lambda: mc.save_snapshot(None))
    expect_exc("save_snapshot('x')",
               lambda: mc.save_snapshot("x"))
    expect_exc("delete_snapshot(None)",
               lambda: mc.delete_snapshot(None))
    expect_exc("delete_snapshot(-9999)  ← nonexistent",
               lambda: mc.delete_snapshot(-9999))

    expect_exc("set_snapshot_scope(None, '/mixer/0/fader')",
               lambda: mc.set_snapshot_scope(None, "/mixer/0/fader"))
    expect_exc("set_snapshot_scope(0, None)",
               lambda: mc.set_snapshot_scope(0, None))
    expect_exc("check_snapshot_scope(None)",
               lambda: mc.check_snapshot_scope(None))

    # ══════════════════════════════════════════════════════════════════════
    # 12. mcp.mix_console — Channel bad inputs
    # ══════════════════════════════════════════════════════════════════════
    section("mcp.mix_console — Channel bad inputs")

    # Append a temp channel to probe; clean up after.
    adv_ch = mc.append_channel()
    note(f"appended temp channel ch={adv_ch.ch}")

    expect_exc("channel.fader = 'x'  ← non-numeric string",
               lambda: setattr(adv_ch, "fader", "x"))
    expect_exc("channel.delay = 'x'",
               lambda: setattr(adv_ch, "delay", "x"))
    expect_exc("channel.name = None",
               lambda: setattr(adv_ch, "name", None))
    expect_exc("channel.name = 42",
               lambda: setattr(adv_ch, "name", 42))

    expect_exc("channel.get_crosspoint(None)",
               lambda: adv_ch.get_crosspoint(None))
    expect_exc("channel.get_crosspoint('x')",
               lambda: adv_ch.get_crosspoint("x"))
    expect_exc("channel.set_crosspoint(None, 0.0)",
               lambda: adv_ch.set_crosspoint(None, 0.0))
    expect_exc("channel.set_crosspoint(0, 'x')",
               lambda: adv_ch.set_crosspoint(0, "x"))

    expect_exc("channel.get_plugin_slot(None)",
               lambda: adv_ch.get_plugin_slot(None))
    expect_exc("channel.get_plugin_slot('x')",
               lambda: adv_ch.get_plugin_slot("x"))
    expect_exc("channel.get_send_slot(None)",
               lambda: adv_ch.get_send_slot(None))
    expect_exc("channel.get_send_slot('x')",
               lambda: adv_ch.get_send_slot("x"))

    # ══════════════════════════════════════════════════════════════════════
    # 13. mcp.mix_console — PluginSlot bad inputs
    # ══════════════════════════════════════════════════════════════════════
    section("mcp.mix_console — PluginSlot bad inputs")

    ps = adv_ch.get_plugin_slot(0)

    expect_exc("plugin_slot.load(None)",
               lambda: ps.load(None))
    expect_exc("plugin_slot.load(42)",
               lambda: ps.load(42))
    expect_exc("plugin_slot.get_param(None)",
               lambda: ps.get_param(None))
    expect_exc("plugin_slot.get_param(42)",
               lambda: ps.get_param(42))
    expect_exc("plugin_slot.set_param(None, 0.0)",
               lambda: ps.set_param(None, 0.0))
    expect_exc("plugin_slot.set_param('id', 'x')",
               lambda: ps.set_param("id", "x"))

    # ══════════════════════════════════════════════════════════════════════
    # 14. mcp.mix_console — SendSlot bad inputs
    # ══════════════════════════════════════════════════════════════════════
    section("mcp.mix_console — SendSlot bad inputs")

    ss = adv_ch.get_send_slot(0)

    expect_exc("send_slot.level = 'x'",
               lambda: setattr(ss, "level", "x"))
    expect_exc("send_slot.engage(None)",
               lambda: ss.engage(None))
    expect_exc("send_slot.engage('x')",
               lambda: ss.engage("x"))

    # Cleanup the temp channel.
    mc.remove_channel(adv_ch)
    note("temp channel removed")

    # ══════════════════════════════════════════════════════════════════════
    # 15. Channel / PluginSlot direct instantiation guard
    # ══════════════════════════════════════════════════════════════════════
    section("mcp.mix_console — direct instantiation guard")

    expect_exc("Channel()  ← direct instantiation forbidden",
               lambda: mc.Channel())
    expect_exc("PluginSlot()  ← direct instantiation forbidden",
               lambda: mc.PluginSlot())
    expect_exc("SendSlot()  ← direct instantiation forbidden",
               lambda: mc.SendSlot())

    # ══════════════════════════════════════════════════════════════════════
    # Report
    # ══════════════════════════════════════════════════════════════════════
    handled = sum(1 for r in results if "✓" in r)
    silent  = sum(1 for r in results if "⚠" in r)
    total   = handled + silent

    summary = f"adversarial_test: {handled}/{total} raised exceptions"
    if silent:
        summary += f"  ({silent} silently ignored bad input)"

    report = summary + "\n" + "\n".join(results)
    print(report)
    mcp.alert(summary)
