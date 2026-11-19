#!/usr/bin/env python
# Runs ON the Yun Shield (its Linux side).
# Fetches /live from Firebase and prints one compact line per ACTIVE person:
#
#   OK|<number of active people>
#   name|exercise|correct|incorrect|percent|last_rep (I or C)|needs_attention|issue|reason
#
# Text is shortened and plain ASCII so the Arduino (2 KB of RAM) can hold it:
# name 10 chars, exercise label 14, issue and reason 24 each, last_rep as I or C.
#
# On a problem it prints a single line:  ERR|<short reason>
#
# Works on both Python 2.7 (what the shield runs) and Python 3.
from __future__ import print_function
import json
import subprocess

DB_URL = "https://cccare-ac2e3-default-rtdb.firebaseio.com"   # no trailing slash
AUTH = ""      # optional: token appended as ?auth=... if your rules need it


TRANSLIT = {
    u"\u00b0": u"",      # degree sign
    u"\u2264": u"<=",    # less-than-or-equal
    u"\u2265": u">=",    # greater-than-or-equal
    u"\u00d7": u"x",     # multiplication sign
    u"\u00b7": u"/",     # middle dot
    u"\u2022": u"/",     # bullet
    u"\u2013": u"-",     # en dash
    u"\u2014": u"-",     # em dash
    u"\u2019": u"'",     # curly apostrophe
}


def clean(value, limit=None):
    # Keep the line format safe: no separators or newlines, plain ASCII only.
    s = u"{0}".format(value)
    for bad, good in TRANSLIT.items():
        s = s.replace(bad, good)
    s = s.replace(u"|", u"/").replace(u"\n", u" ").replace(u"\r", u" ")
    s = s.encode("ascii", "replace").decode("ascii")
    return s[:limit] if limit else s


def exercise_label(raw):
    names = {"squats": u"SQUATS", "jumping_jacks": u"JUMP.JACKS"}
    key = u"{0}".format(raw)
    if key in names:
        return names[key]
    return clean(key.replace(u"_", u" ").upper(), 14)


def rep_code(value):
    # INCORRECT -> I, CORRECT -> C, anything else -> empty
    s = u"{0}".format(value or u"")[:1].upper()
    return s if s in (u"I", u"C") else u""


def num(value):
    try:
        return int(round(float(value)))
    except (TypeError, ValueError):
        return 0


def format_lines(data):
    # Firebase answers {"error": "..."} (a string) when something is wrong.
    if isinstance(data, dict) and len(data) == 1 and "error" in data \
            and not isinstance(data["error"], dict):
        return [u"ERR|" + clean(data["error"])]

    people = []
    if isinstance(data, dict):
        for name in sorted(data.keys()):
            rec = data[name]
            if not isinstance(rec, dict) or rec.get("status") != "active":
                continue
            people.append(u"|".join([
                clean(name, 10),
                exercise_label(rec.get("exercise", "")),
                clean(num(rec.get("correct"))),
                clean(num(rec.get("incorrect"))),
                clean(num(rec.get("percent_correct"))),
                rep_code(rec.get("last_rep")),
                u"1" if rec.get("needs_attention") else u"0",
                clean(rec.get("last_issue", ""), 24),
                clean(rec.get("attention_reason", ""), 24),
            ]))
    return [u"OK|{0}".format(len(people))] + people


def main():
    url = DB_URL + "/live.json"
    if AUTH:
        url += "?auth=" + AUTH
    try:
        raw = subprocess.check_output(["curl", "-s", "-m", "8", url])
    except Exception:
        print("ERR|curl failed")
        return
    try:
        data = json.loads(raw.decode("utf-8"))
    except ValueError:
        print("ERR|bad reply")
        return
    for line in format_lines(data):
        print(line)


if __name__ == "__main__":
    main()
