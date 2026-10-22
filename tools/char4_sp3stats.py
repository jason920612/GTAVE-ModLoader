"""Writes the officer's stats definition (research/char4-design.md §3.7, "SP3 stats"): every characterStat of the SP
stats setup becomes an explicit SP3_<name> with the same type, packing, save category and owner. profile is always
false (the protagonists' profile copies stay theirs) and characterStat false (the engine would expand it to SP0-SP2).
Label stats are left out: declaring SP3_MISSION_NAME / SP3_LAST_MISSION_NAME (Type="label") makes the game crash when
story mode starts (M0, research/phase0.md §55.6), and the officer plays no missions.
usage: char4_sp3stats.py <spstatssetup.xml> <out.xml>"""
import re, sys

KEEP = ('Type', 'packed', 'SaveCategory', 'online', 'Owner', 'FlushPriority')


def main():
    text = open(sys.argv[1], encoding='utf-8', errors='replace').read()
    out = ['<StatsSetup version="1">', '    <stats>']
    for m in re.finditer(r'<stat\s([^>]*?)/?>', text):
        attrs = dict(re.findall(r'(\w+)="([^"]*)"', m.group(1)))
        if attrs.get('characterStat') != 'true' or attrs.get('Type') == 'label':
            continue
        parts = ['Name="SP3_%s"' % attrs['Name']] + ['%s="%s"' % (k, attrs[k]) for k in KEEP if k in attrs]
        out.append('\t\t<stat %s profile="false" characterStat="false" />' % ' '.join(parts))
    out += ['\t</stats>', '</StatsSetup>', '']
    open(sys.argv[2], 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    print('%d SP3 stats written' % (len(out) - 5))


if __name__ == '__main__':
    main()
