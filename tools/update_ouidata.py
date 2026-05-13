#!/usr/bin/env python3
import argparse
import csv
import io
import json
import re
import sys
from pathlib import Path
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "luci-app-netmon/htdocs/luci-static/resources/view/netmon/ouidata.json"

IEEE_SOURCES = [
    ("https://standards-oui.ieee.org/oui/oui.csv", 24),
    ("https://standards-oui.ieee.org/oui28/mam.csv", 28),
    ("https://standards-oui.ieee.org/oui36/oui36.csv", 36),
]
WIRESHARK_MANUF = "https://www.wireshark.org/download/automated/data/manuf"

KEEP_FIELDS = ("hostname",)

ALIASES = [
    (re.compile(r"\bApple,\s*Inc\.", re.I), "Apple"),
    (re.compile(r"\bSamsung Electronics\b.*", re.I), "Samsung"),
    (re.compile(r"\bHuawei Technologies\b.*", re.I), "Huawei"),
    (re.compile(r"\bXiaomi Communications\b.*", re.I), "Xiaomi"),
    (re.compile(r"\bBeijing Xiaomi\b.*", re.I), "Xiaomi"),
    (re.compile(r"\bGoogle,\s*Inc\.", re.I), "Google"),
    (re.compile(r"\bAmazon Technologies\b.*", re.I), "Amazon"),
    (re.compile(r"\bRaspberry Pi\b.*", re.I), "Raspberry Pi"),
    (re.compile(r"\bVMware,\s*Inc\.", re.I), "VMware"),
]


def fetch_text(url):
    request = Request(
        url,
        headers={
            "User-Agent": "netmon-ouidata-updater/1.0",
            "Accept": "text/csv,text/plain,*/*",
        },
    )
    with urlopen(request, timeout=30) as res:
        return res.read().decode("utf-8", "replace")


def clean_prefix(value):
    return re.sub(r"[^0-9A-Fa-f]", "", value or "").upper()


def clean_vendor(value):
    vendor = re.sub(r"\s+", " ", value or "").strip()
    vendor = re.sub(
        r"\b(Co\.?,?\s*Ltd\.?|Corporation|Corp\.?|Incorporated|Inc\.?|Limited|Ltd\.?|Technology|Technologies)\b\.?",
        "",
        vendor,
        flags=re.I,
    )
    vendor = re.sub(r"\s*,\s*$", "", vendor).strip()

    for pattern, replacement in ALIASES:
        if pattern.search(vendor):
            return replacement

    return vendor


def add_prefix(groups, prefix, vendor):
    prefix = clean_prefix(prefix)
    vendor = clean_vendor(vendor)
    if not prefix or not vendor:
        return

    groups.setdefault(str(len(prefix)), {})[prefix] = vendor


def parse_ieee_csv(text, expected_bits, groups):
    reader = csv.DictReader(io.StringIO(text))
    prefix_len = expected_bits // 4

    for row in reader:
        assignment = row.get("Assignment") or row.get("Registry") or row.get("MAC Prefix") or ""
        vendor = row.get("Organization Name") or row.get("Organization") or ""
        prefix = clean_prefix(assignment)[:prefix_len]
        add_prefix(groups, prefix, vendor)


def parse_wireshark_manuf(text, groups):
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue

        parts = line.split(None, 2)
        if len(parts) < 2:
            continue

        prefix_token = parts[0]
        prefix = clean_prefix(prefix_token.split("/")[0])
        if "/" in prefix_token:
            try:
                prefix = prefix[:(int(prefix_token.split("/", 1)[1]) + 3) // 4]
            except ValueError:
                pass

        vendor = parts[2] if len(parts) > 2 else parts[1]
        add_prefix(groups, prefix, vendor)


def load_existing(path):
    if not path.exists():
        return {}

    with path.open("r", encoding="utf-8") as fp:
        return json.load(fp)


def merge_existing(groups, existing):
    for prefix, vendor in (existing.get("oui") or {}).items():
        add_prefix(groups, prefix, vendor)

    for prefixes in (existing.get("prefixes") or {}).values():
        for prefix, vendor in prefixes.items():
            add_prefix(groups, prefix, vendor)


def build_database(existing, groups):
    data = {
        "version": 2,
        "prefixes": {
            length: dict(sorted(prefixes.items()))
            for length, prefixes in sorted(groups.items(), key=lambda item: int(item[0]), reverse=True)
        }
    }

    for field in KEEP_FIELDS:
        if field in existing:
            data[field] = existing[field]

    return data


def main():
    parser = argparse.ArgumentParser(description="Update netmon OUI database")
    parser.add_argument("-o", "--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--offline", action="store_true", help="only normalize and re-emit existing database")
    args = parser.parse_args()

    existing = load_existing(args.output)
    groups = {}

    if args.offline:
        merge_existing(groups, existing)
    else:
        for url, bits in IEEE_SOURCES:
            print(f"fetch {url}", file=sys.stderr)
            try:
                parse_ieee_csv(fetch_text(url), bits, groups)
            except Exception as exc:
                print(f"warning: skip {url}: {exc}", file=sys.stderr)

        print(f"fetch {WIRESHARK_MANUF}", file=sys.stderr)
        try:
            parse_wireshark_manuf(fetch_text(WIRESHARK_MANUF), groups)
        except Exception as exc:
            print(f"warning: skip {WIRESHARK_MANUF}: {exc}", file=sys.stderr)

    data = build_database(existing, groups)
    args.output.parent.mkdir(parents=True, exist_ok=True)

    with args.output.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(data, fp, ensure_ascii=False, indent=4)
        fp.write("\n")

    total = sum(len(items) for items in data["prefixes"].values())
    print(f"wrote {args.output} ({total} prefixes)", file=sys.stderr)


if __name__ == "__main__":
    main()
