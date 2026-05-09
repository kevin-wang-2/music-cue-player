"""
csv_import — import cue skeleton from a CSV file.

Each row becomes a Group cue in the active cue list.

CSV format (default — first two columns):
    col 0: cue number
    col 1: cue name

Usage (from a scriptlet):
    import mcp.library.csv_import as csv_import
    csv_import.run()                              # opens file dialog
    csv_import.run("/path/to/file.csv")
    csv_import.run(path, delimiter=";", has_header=False)
"""

import csv


def run(path=None, delimiter=",", has_header=True, col_number=0, col_name=1):
    """Import cue skeleton from a CSV file.

    Args:
        path:        Path to CSV file; opens a file dialog if None.
        delimiter:   CSV field delimiter (default ",").
        has_header:  Skip the first row when True (default True).
        col_number:  Column index for cue number (default 0).
        col_name:    Column index for cue name   (default 1).
    """
    import mcp
    import mcp.cue_list

    # ── file selection ────────────────────────────────────────────────────
    if path is None:
        path = mcp.file(
            title="Import CSV as Cue Skeleton",
            mode="open",
            filter="CSV files (*.csv);;All files (*)",
        )
        if not path:
            return  # user cancelled

    # ── parse ─────────────────────────────────────────────────────────────
    rows = []
    try:
        with open(path, newline="", encoding="utf-8-sig") as fh:
            reader = csv.reader(fh, delimiter=delimiter)
            for i, row in enumerate(reader):
                if i == 0 and has_header:
                    continue
                if not any(cell.strip() for cell in row):
                    continue  # skip blank lines
                num  = row[col_number].strip() if col_number < len(row) else ""
                name = row[col_name].strip()   if col_name  < len(row) else ""
                rows.append((num, name))
    except Exception as exc:
        mcp.alert(f"Error reading CSV:\n{exc}")
        return

    if not rows:
        mcp.alert("No data rows found in the CSV file.")
        return

    # ── insert group cues ────────────────────────────────────────────────
    cl = mcp.cue_list.get_active_cue_list()
    created = 0
    errors  = []

    for num, name in rows:
        try:
            cl.insert_cue("group", cuenumber=num, cuename=name)
            created += 1
        except Exception as exc:
            errors.append(f"  {num or '(no number)'}: {exc}")

    # ── report ────────────────────────────────────────────────────────────
    msg = f"Imported {created} group cue(s)."
    if errors:
        msg += f"\n\nFailed ({len(errors)}):\n" + "\n".join(errors[:10])
        if len(errors) > 10:
            msg += f"\n  … and {len(errors) - 10} more"
    mcp.alert(msg)
