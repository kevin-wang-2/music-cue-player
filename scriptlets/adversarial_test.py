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
    note(f"test list deleted")

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
