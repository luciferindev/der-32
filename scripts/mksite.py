#!/usr/bin/env python3
"""Generate the secondary pages of the DER/32 website.

site/index.html is edited by hand.  This script takes its <head>, header,
sidebar and footer and builds the other pages around them, so every page
shares one layout:

    install.html  faq.html  cheatsheet.html  man.html  man-upkg.8.html
    man-derfetch.1.html  slow.html  404.html  rss.xml  robots.txt  humans.txt

Man pages are rendered from upkg/doc/upkg.8 and derfetch/derfetch.1 with
mandoc (out/tools/bin/nbmandoc from the build, or a host mandoc).

    python3 scripts/mksite.py
"""
import html
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SITE = os.path.join(ROOT, "site")
URL = "https://luciferindev.github.io/der-32/"
GH = "https://github.com/luciferindev/der-32"

TABS = [
    ("index.html", "Home"),
    ("index.html#news", "News"),
    ("install.html", "Install"),
    ("faq.html", "FAQ"),
    ("cheatsheet.html", "Cheat sheet"),
    ("man.html", "Man pages"),
    ("index.html#history", "History"),
    (GH + "/tree/main/docs", "Docs"),
    (GH + "/issues", "Bugs"),
    (GH, "Source"),
]


def tabbar(active):
    out = ['<div id="tabs">']
    for href, label in TABS:
        cls = ' class="on"' if href == active else ""
        out.append(f'  <a{cls} href="{href}">{label}</a>')
    out.append("</div>")
    return "\n".join(out)


def between(s, a, b, inclusive=True):
    i = s.index(a)
    j = s.index(b, i) + len(b)
    return s[i:j] if inclusive else s[i + len(a):j - len(b)]


INDEX_PATH = os.path.join(SITE, "index.html")
index = open(INDEX_PATH).read()

# Keep the hand-written index's tab bar in step with the shared one.
index = re.sub(r'<div id="tabs">.*?</div>', lambda m: tabbar("index.html"),
               index, count=1, flags=re.S)
open(INDEX_PATH, "w").write(index)

HEAD = index[: index.index("<title>")]
HEAD_TAIL = between(index, '<link rel="icon"', "</head>")
HEADER = between(index, '<div id="head">', "</div>\n</div>")
SIDE = between(index, '<div id="side">', "</div>\n\n<div id=\"main\">")
SIDE = SIDE[: -len('\n\n<div id="main">')]
FOOT = between(index, '<div id="badges">', "</html>")


def page(name, title, active, body, desc=None, base=None):
    desc = desc or title
    head = HEAD + (f'<base href="{base}">\n' if base else "")
    doc = (head
           + f"<title>{title} :: DER/32</title>\n"
           + f'<meta name="description" content="{html.escape(desc)}">\n'
           + HEAD_TAIL + "\n<body>\n<div id=\"wrap\">\n\n"
           + HEADER + "\n\n" + tabbar(active) + "\n\n"
           + '<div id="cols">\n\n' + SIDE + "\n\n<div id=\"main\">\n"
           + body + "\n</div>\n</div>\n\n" + FOOT + "\n")
    open(os.path.join(SITE, name), "w").write(doc)
    print("wrote site/" + name)


def term(text):
    """Black terminal box; '$ ' and '# ' prompts and '# comments' coloured."""
    lines = []
    for raw in text.strip("\n").split("\n"):
        line = html.escape(raw)
        m = re.match(r"^(\$|der32#) (.*)$", line)
        if m:
            cmd, cmt = m.group(2), ""
            if "  # " in cmd:
                cmd, c = cmd.split("  # ", 1)
                cmt = '  <span class="c"># ' + c + "</span>"
            line = f'<span class="p">{m.group(1)}</span> {cmd}{cmt}'
        lines.append(line)
    return '<div class="term">' + "\n".join(lines) + "</div>"


def ref(rows, head=("Command", "What it does")):
    out = [f'<table class="ref"><tr><th>{head[0]}</th><th>{head[1]}</th></tr>']
    for a, b in rows:
        out.append(f"<tr><td>{html.escape(a)}</td><td>{b}</td></tr>")
    out.append("</table>")
    return "\n".join(out)


def post(title, body, meta=None, icon="?", anchor=None):
    a = f'<a id="{anchor}"></a>\n' if anchor else ""
    m = f'<div class="meta">{meta}</div>' if meta else ""
    return (f'{a}<div class="post"><div class="t"><span class="ico">{icon}</span> '
            f"{title}</div>{m}<div class=\"b\">{body}</div></div>")


def qa(q, a):
    return f'<div class="qa"><div class="q">{q}</div><div class="a">{a}</div></div>'


# ---------------------------------------------------------------- FAQ

faq = [
    post("Frequently Asked Questions", """
<p>Read this before posting a bug. Everything here applies to <b>DER/32 0.1 &ldquo;Genesis&rdquo;</b>.</p>
<p>Jump to: <a href="#general">General</a> &middot; <a href="#hardware">Hardware</a> &middot;
<a href="#using">Using it</a> &middot; <a href="#packages">Packages</a> &middot;
<a href="#build">Build problems</a> &middot; <a href="#contrib">Contributing</a></p>""",
         meta="Sticky &middot; Forum: General", icon="!"),

    '<a id="general"></a><h3 class="crumb">:: General</h3>',
    qa("Is DER/32 a Linux distro?",
       "<p>No. A distro is a kernel plus a pile of packages that someone hopes will get along. DER/32 is one "
       "operating system: the kernel and the userland come from one NetBSD source tree, with BSD libc, BSD make, "
       "BSD init and rc.d, POSIX <tt>sh</tt> and FFS. Linux is only the <i>build host</i>. It's very good at "
       "compiling BSD.</p>"),
    qa("Is it GNU-free?",
       "<p>No, and that's on purpose. Like NetBSD, the base system ships GCC and binutils as its compiler "
       "toolchain. Everything else in base is BSD-licensed.</p>"),
    qa("Why not just run NetBSD?",
       "<p>For a production machine, you probably should. DER/32 is a derivative: it pins one "
       "<tt>netbsd-10</tt> commit, adds a small patch series (its own identity, kernel config, images, "
       "<tt>upkg</tt>, <tt>derfetch</tt>) and a slimmer build profile. If you want a hackable BSD where "
       "every change from upstream fits in a handful of reviewable patches, that's what it's for.</p>"),
    qa("Isn't this basically MINIX?",
       "<p>No. MINIX is a microkernel teaching system, and these days it mostly lives inside Intel's "
       "Management Engine, where you can't log in to it. DER/32 is a monolithic BSD kernel that you can. "
       "(Tanenbaum and Torvalds argued about this in 1992. The tortoise abstained.)</p>"),
    qa("Why does <tt>uname -s</tt> say DER32 but <tt>uname -r</tt> says 10.2?",
       "<p>The system name is DER/32's own. The release number is NetBSD's, kept on purpose so kernel "
       "modules, binary packages and anything else that checks the ABI version keep working. DER/32's "
       "own release is in <tt>sysctl kern.der32</tt>.</p>"),


    '<a id="hardware"></a><h3 class="crumb">:: Hardware</h3>',
    qa("Will it run on my machine?",
       "<p>DER/32 uses NetBSD 10's <tt>GENERIC</tt> kernel configuration, so it has the same drivers as "
       "NetBSD 10 on amd64. NetBSD's hardware lists are the best guide. So far, Genesis has only been "
       "tested in QEMU. Reports from real hardware are welcome on the "
       f'<a href="{GH}/issues">bug tracker</a>.</p>'),
    qa("What are the requirements?",
       "<p>An x86-64 CPU. The automated tests boot with 1&nbsp;GiB of RAM and 2 CPUs. The disk image is "
       "2&nbsp;GiB and the live ISO about 525&nbsp;MiB. The live ISO copies <tt>/etc</tt>, <tt>/var</tt>, "
       "<tt>/root</tt> and <tt>/usr/local</tt> into RAM, so give it some headroom.</p>"),
    qa("I only see a blank screen / no login on my monitor.",
       "<p>The Genesis images use a <b>serial console</b> (<tt>com0</tt>, getty on <tt>tty00</tt>). "
       "In QEMU that's your terminal. On real hardware you need a serial cable, or the VGA-console "
       f'image variant that is being worked on in <a href="{GH}/pull/1">pull request #1</a>.</p>'),
    qa("How do I install it to a disk?",
       '<p>See the <a href="install.html">install guide</a>. Short version: there\'s no installer yet. '
       "You write the disk image to a disk, and that <b>erases the disk</b>.</p>"),

    '<a id="using"></a><h3 class="crumb">:: Using it</h3>',
    qa("What's the login?",
       "<p><tt>root</tt>, password <tt>der32</tt>. Change it right away with <tt>passwd</tt>. "
       "A first-boot password prompt is planned.</p>"),
    qa("How do I get out of QEMU?",
       "<p>Press <b>Ctrl-A</b>, then <b>X</b>. To shut down cleanly first, run "
       "<tt>shutdown -p now</tt> inside DER/32.</p>"),
    qa("Is networking on?",
       "<p>Yes. <tt>dhcpcd</tt> runs at boot and configures every interface. In QEMU, user-mode "
       "networking gives the guest outbound access. SSH is off by default: set <tt>sshd=YES</tt> in "
       "<tt>/etc/rc.conf</tt> and run <tt>service sshd start</tt>.</p>"),
    qa("Where's X11 / a desktop?",
       "<p>The Genesis build profile leaves X11 out. DER/32 0.1 is a console system.</p>"),
    qa("How do I change the system's settings?",
       "<p>The NetBSD way: <tt>/etc/rc.conf</tt> for services, <tt>/etc/sysctl.conf</tt> for kernel "
       "tunables, <tt>/etc/fstab</tt> for mounts. See the <a href=\"cheatsheet.html\">cheat sheet</a>.</p>"),

    '<a id="packages"></a><h3 class="crumb">:: Packages</h3>',
    qa("upkg or pkgin, which one do I use?",
       "<p><b>upkg</b> is DER/32's own package manager. It installs from DER/32's signed repositories into "
       "<tt>/usr/local</tt>. <b>pkgin</b> installs NetBSD's pkgsrc binary packages into <tt>/usr/pkg</tt>. "
       "The two never touch each other's files.</p>"),
    qa("Will NetBSD binaries run on DER/32?",
       "<p>They should. DER/32 keeps the NetBSD 10 ABI: same libc, same syscalls, same "
       "<tt>kern.osrelease</tt>.</p>"),
    qa("Does upkg have Super Cow Powers?",
       "<p>No. Ask it about cows and find out what it has instead.</p>"),
    qa("Does upkg run install scripts?",
       "<p>Never. A package is files plus metadata, and that's all upkg installs. Index signatures are "
       "Ed25519, package checksums are SHA-256, and an interrupted transaction is completed or rolled back "
       "on the next run. Details are in <a href=\"man-upkg.8.html\">upkg(8)</a>.</p>"),
    qa("How do I make my own package?",
       "<p>Write a recipe under <tt>ports/&lt;category&gt;/&lt;name&gt;/</tt>: a <tt>RECIPE</tt> file and a "
       "<tt>build</tt> script, then run <tt>upkg build &lt;name&gt;</tt>. Look at <tt>ports/core/lua</tt> "
       f'for a real example, and <a href="{GH}/blob/main/docs/packages.md">packages.md</a> for the format.</p>'),

    '<a id="build"></a><h3 class="crumb">:: Build problems</h3>',
    qa("<tt>make</tt> stops right away without an error message.",
       "<p>You're on an old checkout: early versions of the scripts failed silently on a fresh clone. "
       "Run <tt>git pull</tt> and <tt>git submodule update --init</tt>, then try again.</p>"),
    qa("The tools build fails in <tt>gmake</tt> / <tt>ar.c</tt> with GCC 14 or newer.",
       "<p>GCC 14+ defaults to C23 and turns old C warnings into errors, which breaks NetBSD's bundled host "
       "tools. Current DER/32 probes the compiler and passes the right flags, and it rebuilds the host tools "
       "automatically when those flags change. Run <tt>git pull</tt> and <tt>make release</tt> again. You don't "
       "need to delete <tt>out/</tt>.</p>"),
    qa("How much disk does a build need?",
       "<p>About 20&nbsp;GB: the NetBSD source tree is around 3&nbsp;GB and <tt>out/</tt> around 14&nbsp;GB "
       "after a full release build.</p>"),
    qa("Can I resume a build that stopped?",
       "<p>Yes. Just run the same <tt>make</tt> target again. Builds are incremental "
       "(<tt>build.sh -u</tt>).</p>"),

    '<a id="contrib"></a><h3 class="crumb">:: Contributing</h3>',
    qa("How do I change something in the OS itself?",
       "<p>DER/32 never edits the NetBSD tree in place. Make your change inside <tt>src/</tt> on the "
       "<tt>der32</tt> branch, commit it there, then regenerate the patch series:</p>"
       + term("$ scripts/patches.sh status  # pinned commit and patch state\n"
              "$ scripts/patches.sh apply   # pinned NetBSD + patches/ on branch der32\n"
              "$ scripts/patches.sh export  # write your commits back to patches/")
       + f'<p>Then open a <a href="{GH}/pulls">pull request</a> with the new or changed file in '
       "<tt>patches/</tt>.</p>"),
    qa("What's the license?",
       "<p>DER/32's own code is BSD-2-Clause. NetBSD code keeps its original licences and notices. See "
       f'<a href="{GH}/tree/main/LICENSES">LICENSES/</a>.</p>'),
]
page("faq.html", "FAQ", "faq.html", "\n".join(faq),
     "Frequently asked questions about DER/32: hardware, installing, packages, build problems.")

# ---------------------------------------------------------------- cheat sheet

cs = [
    post("DER/32 cheat sheet", "<p>The commands you'll want in the first hour. Print it out and tape it to "
         "your monitor. Jump to: <a href=\"#c-upkg\">upkg</a> &middot; <a href=\"#c-pkgin\">pkgin</a> &middot; "
         "<a href=\"#c-sys\">system</a> &middot; <a href=\"#c-svc\">services</a> &middot; "
         "<a href=\"#c-net\">network</a> &middot; <a href=\"#c-users\">users</a> &middot; "
         "<a href=\"#c-build\">building</a></p>", meta="Sticky &middot; Forum: HOWTOs", icon="!"),

    post("upkg: DER/32 packages", ref([
        ("upkg sync", "fetch and verify every enabled repository index"),
        ("upkg search lua", "search package names and descriptions"),
        ("upkg info lua", "installed and repository information"),
        ("upkg install lua-hello", "install with dependencies (<tt>-n</tt>: dry run)"),
        ("upkg install core/lua", "take the package from one repository only"),
        ("upkg install 'lua>=5.4'", "version constraint: <tt>&lt; &lt;= = &gt;= &gt;</tt>"),
        ("upkg upgrade", "upgrade everything that has a newer version"),
        ("upkg remove lua-hello", "remove (refused if something depends on it)"),
        ("upkg remove -c lua", "remove together with its dependents"),
        ("upkg autoremove", "drop dependencies nothing needs any more"),
        ("upkg list -e", "explicitly installed packages (<tt>-d</tt>: dependencies)"),
        ("upkg list -f lua", "files owned by a package"),
        ("upkg check", "verify installed files against their checksums"),
        ("upkg build der32-hello", "build a package from a recipe in <tt>ports/</tt>"),
        ("upkg repo list", "repositories, priorities, enabled state"),
        ("upkg repo add web URL -p 90", "add a repository with priority 90"),
        ("upkg recover", "finish or undo an interrupted transaction"),
    ]) + '<p>Full reference: <a href="man-upkg.8.html">upkg(8)</a>.</p>', icon="P", anchor="c-upkg"),

    post("pkgin: NetBSD binary packages", ref([
        ("pkg_add pkgin", "one-time bootstrap from cdn.NetBSD.org"),
        ("pkgin update", "refresh the package list"),
        ("pkgin search vim", "search"),
        ("pkgin install vim git", "install with dependencies, into <tt>/usr/pkg</tt>"),
        ("pkgin upgrade", "upgrade installed packages"),
        ("pkgin remove vim", "remove"),
        ("pkgin autoremove", "remove orphaned dependencies"),
        ("pkgin list", "installed packages"),
        ("pkg_info", "installed packages, the low-level way"),
    ]), icon="P", anchor="c-pkgin"),

    post("System", ref([
        ("derfetch", "system info and the tortoise (<tt>-l</tt> no logo, <tt>-n</tt> no colour)"),
        ("uname -a", "kernel name, release, version"),
        ("sysctl kern.der32", "DER/32 release name and codename"),
        ("cat /etc/release", "release and build information"),
        ("dmesg | less", "kernel boot messages: what hardware was found"),
        ("top", "processes"),
        ("df -h", "disk space"),
        ("sysctl hw.physmem64", "installed memory, in bytes"),
        ("shutdown -p now", "power off"),
        ("shutdown -r now", "reboot"),
        ("certctl rehash", "rebuild <tt>/etc/openssl/certs</tt> (HTTPS trust)"),
    ]), icon="S", anchor="c-sys"),

    post("Services (rc.d)", ref([
        ("vi /etc/rc.conf", "turn services on and off: <tt>sshd=YES</tt>"),
        ("service sshd start", "start a service now (also <tt>stop</tt>, <tt>restart</tt>, <tt>status</tt>)"),
        ("service -l", "list every rc.d script"),
        ("service sshd onestart", "start once without enabling it in rc.conf"),
        ("ls /etc/rc.d", "the service scripts themselves"),
    ]), icon="S", anchor="c-svc"),

    post("Network", ref([
        ("ifconfig -a", "interfaces and addresses"),
        ("service dhcpcd restart", "redo DHCP on every interface"),
        ("route -n show", "routing table"),
        ("cat /etc/resolv.conf", "DNS servers (written by dhcpcd)"),
        ("ping -c 3 netbsd.org", "check connectivity"),
        ("ftp https://example.org/file", "download a file (base has no curl or wget)"),
    ]), icon="N", anchor="c-net"),

    post("Users", ref([
        ("passwd", "change your password (do this first as root)"),
        ("useradd -m -G wheel alice", "add a user with a home directory, allowed to <tt>su</tt>"),
        ("passwd alice", "set their password"),
        ("su -", "become root (users in group wheel only)"),
        ("userdel -r alice", "remove a user and their home"),
    ]), icon="U", anchor="c-users"),

    post("Building DER/32 (on the Linux host)", ref([
        ("make check", "check the host for required tools"),
        ("make release", "tools, kernel, world, sets, disk image"),
        ("make iso", "live ISO"),
        ("make qemu", "boot the image here, serial console (exit: Ctrl-A X)"),
        ("SNAPSHOT=1 make qemu", "boot without keeping any disk changes"),
        ("make test", "every test suite"),
        ("make test-upkg", "only the upkg suite (fast, no NetBSD build needed)"),
        ("make kernel", "just the DER32 kernel"),
        ("scripts/patches.sh status", "pinned NetBSD commit and patch state"),
        ("make clean", "remove build output"),
    ], head=("Target", "What it does")), icon="B", anchor="c-build"),
]
page("cheatsheet.html", "Cheat sheet", "cheatsheet.html", "\n".join(cs),
     "DER/32 command cheat sheet: upkg, pkgin, rc.d services, networking, users, building.")

# ---------------------------------------------------------------- man pages

MANDOC = shutil.which("mandoc") or os.path.join(ROOT, "out/tools/bin/nbmandoc")
MANS = [("upkg", "8", "upkg/doc/upkg.8", "DER/32 package manager"),
        ("derfetch", "1", "derfetch/derfetch.1", "show system information next to Genesis the tortoise")]
if not os.access(MANDOC, os.X_OK):
    sys.exit("mksite: no mandoc (install mandoc, or build tools with 'make tools')")
for name, sec, src, desc in MANS:
    frag = subprocess.check_output(
        [MANDOC, "-T", "html", "-O", "fragment,man=man-%N.%S.html",
         os.path.join(ROOT, src)], text=True)
    body = post(f"{name}({sec}) &mdash; {desc}",
                f'<p class="crumb"><a href="man.html">Man pages</a> &raquo; {name}({sec})</p>'
                f'<div class="man">{frag}</div>', icon="M")
    page(f"man-{name}.{sec}.html", f"{name}({sec})", "man.html", body, f"{name}({sec}): {desc}")

rows = "\n".join(
    f'<tr><td class="fi"><span class="ico">M</span></td><td><a href="man-{n}.{s}.html"><b>{n}({s})</b></a></td>'
    f"<td><small>{html.escape(d)}</small></td></tr>" for n, s, _, d in MANS)
page("man.html", "Man pages", "man.html",
     post("Manual pages", "<p>DER/32's own manual pages. On a running system, type <tt>man upkg</tt>. "
          "Everything inherited from NetBSD is documented at "
          '<a href="https://man.netbsd.org/">man.netbsd.org</a>.</p>', icon="M")
     + f'\n<table class="idx"><tr><th></th><th>Page</th><th>Description</th></tr>\n{rows}\n</table>',
     "DER/32 manual pages: upkg(8), derfetch(1).")

# ---------------------------------------------------------------- install guide

inst = [
    post("Install guide", """
<p>DER/32 0.1 doesn't have an installer. There's no wizard, no partition editor and no screen asking you to
pick from twelve desktop environments. The disk image <i>is</i> the installed system: you boot it in a VM, or
write it to a disk.</p>
<p>Pick one: <a href="#vm">A. QEMU (recommended)</a> &middot; <a href="#live">B. live ISO</a> &middot;
<a href="#metal">C. real hardware</a>. Then do <a href="#after">the first-boot checklist</a>.</p>
<div class="quote"><b>Before you start</b>Prebuilt images aren't published yet, so you build them first. That
takes a Linux host with about 20&nbsp;GB free and a C compiler, and an hour or two the first time.
Arch is the reference host.</div>""", meta="Sticky &middot; Forum: Installation", icon="!"),

    post("Step 0: build the images", term("""
$ git clone --recurse-submodules https://github.com/luciferindev/der-32
$ cd der-32
$ make check  # are the host tools there?
$ make release  # disk image: out/images/der32-0.1-amd64.img
$ make iso  # live ISO: out/images/der32-0.1-amd64.iso""")
         + "<p>A stopped build resumes where it left off: run the same command again. Stuck? "
           'See <a href="faq.html#build">build problems</a>.</p>', icon="0"),

    post("A. Run it in QEMU (recommended)", "<p>This is how DER/32 is developed and tested.</p>"
         + term("""
$ sudo pacman -S qemu-base  # Arch; other distros: install qemu-system-x86_64
$ make qemu  # boots a working copy of the image on this terminal""")
         + "<p>Your changes persist in <tt>out/images/der32-run.img</tt>, so the release image stays clean. "
           "Delete that file to start over. Useful knobs:</p>"
         + ref([("MEM=2048 make qemu", "more RAM (default 1024 MiB)"),
                ("SMP=4 make qemu", "more CPUs (default 2)"),
                ("SNAPSHOT=1 make qemu", "throw away every change on exit"),
                ("Ctrl-A, then X", "quit QEMU (run <tt>shutdown -p now</tt> first)")],
               head=("Do this", "To get")), icon="A", anchor="vm"),

    post("B. Boot the live ISO", "<p>The live ISO runs from read-only media and keeps <tt>/etc</tt>, "
         "<tt>/var</tt>, <tt>/root</tt> and <tt>/usr/local</tt> in RAM, so nothing survives a reboot. "
         "That's ideal for a look around.</p>"
         + term("""
$ qemu-system-x86_64 -accel kvm -cpu host -m 1024 \\
    -cdrom out/images/der32-0.1-amd64.iso -boot d \\
    -display none -serial stdio""")
         + "<p>Drop <tt>-accel kvm -cpu host</tt> if you have no <tt>/dev/kvm</tt>. The ISO is built as "
           "a CD image. Booting it from a USB stick hasn't been tested.</p>", icon="B", anchor="live"),

    post("C. Real hardware", """
<div class="quote"><b>Read this twice</b>Writing the image <b>destroys everything on the target disk</b>.
And Genesis talks to a <b>serial port</b>, not your monitor: without a serial cable you'll see the
bootloader and then nothing. A VGA-console image is in the works in
<a href="https://github.com/luciferindev/der-32/pull/1">pull request #1</a>. Real hardware hasn't been
tested yet.</div>"""
         + term("""
$ lsblk -o NAME,SIZE,MODEL  # identify the target disk. Twice.
$ sudo dd if=out/images/der32-0.1-amd64.img of=/dev/sdX bs=4M conv=fsync status=progress
$ sync""")
         + "<p>Boot it with the BIOS / legacy boot option (the image has an MBR, not UEFI). Connect at "
           "9600 baud. On the Linux side: <tt>screen /dev/ttyUSB0 9600</tt>.</p>"
           "<p>The image's filesystem is 2&nbsp;GiB, whatever the size of the disk. Growing it isn't "
           "documented yet.</p>", icon="C", anchor="metal"),

    post("After the first boot", "<p>Log in as <tt>root</tt> with password <tt>der32</tt>. Then:</p>"
         + term("""
der32# passwd  # first. Seriously.
der32# echo 'hostname=tortoise' >> /etc/rc.conf
der32# hostname tortoise
der32# ln -sf /usr/share/zoneinfo/America/Boise /etc/localtime  # your zone
der32# useradd -m -G wheel alice  # a user who may su to root
der32# passwd alice
der32# upkg sync  # DER/32 packages
der32# derfetch  # admire the tortoise""")
         + "<p>Want SSH? Add <tt>sshd=YES</tt> to <tt>/etc/rc.conf</tt> and run <tt>service sshd start</tt>. "
           'More commands are on the <a href="cheatsheet.html">cheat sheet</a>. Your Linux muscle memory mostly '
           "works, and where it doesn't, <tt>man</tt> works.</p>", icon="!", anchor="after"),
]
page("install.html", "Install guide", "install.html", "\n".join(inst),
     "How to install DER/32: build the images, run it in QEMU, boot the live ISO, or write it to a disk.")

# ---------------------------------------------------------------- slow lane (secret)

def pre_art(lines):
    return html.escape("\n".join(lines))

ART = [l.replace("$1", "").replace("$2", "")
       for l in open(os.path.join(ROOT, "share/fastfetch/der32.txt")).read().rstrip("\n").split("\n")]
HARE = ['  zZz', '   (\\_/)', '   (-.-)', '  o(_(")(")']
page("slow.html", "The slow lane", "", post(
    "You found the slow lane",
    "<p>The hare had a head start, a faster kernel, and a new distro every weekend. Watch.</p>"
    '<div class="track"><pre class="finish">F\nI\nN\nI\nS\nH</pre>'
    f'<pre class="hare">{pre_art(HARE)}</pre>'
    f'<pre class="runner">{pre_art(ART)}</pre></div>'
    "<p>Slow and steady boots the kernel. More hidden things live on a running DER/32 system. "
    "Start with <tt>upkg tortoise</tt>.</p>"
    '<p><a href="index.html">&laquo; Back to the fast lane</a></p>', icon="T"))

open(os.path.join(SITE, "robots.txt"), "w").write(
    "# Tortoises welcome. Hares, see below.\n"
    "User-agent: *\nAllow: /\n\n"
    "User-agent: hare\nDisallow: /\n")
open(os.path.join(SITE, "humans.txt"), "w").write(
    "/* TEAM */\n  Project: luciferindev\n  Mascot: Genesis (tortoise, bowler hat)\n"
    "  Site: https://github.com/luciferindev/der-32\n\n"
    "/* SITE */\n  Standards: HTML5, CSS\n  JavaScript: none\n  Best viewed in: lynx\n"
    "  Built with: a text editor and a tortoise's patience\n")
print("wrote site/robots.txt site/humans.txt")

# ---------------------------------------------------------------- 404

page("404.html", "Not found", "", post(
    "404: No such file or directory",
    term("der32# cat /this/page\ncat: /this/page: No such file or directory\nder32# _")
    + '<p>Genesis looked everywhere. Slowly. That page isn\'t here. Try the <a href="index.html">front page</a>, the '
      '<a href="faq.html">FAQ</a> or the <a href="cheatsheet.html">cheat sheet</a>.</p>', icon="!"),
    # GitHub Pages serves 404.html at whatever path was asked for.
    base="/der-32/")

# ---------------------------------------------------------------- RSS

NEWS = [
    ("NetBSD binary packages on DER/32", "index.html#news", "Fri, 20 Nov 2026 00:15:00 -0700",
     "DER/32 keeps the NetBSD 10 ABI, so NetBSD's pkgsrc binary packages install unmodified under "
     "/usr/pkg with pkg_add and pkgin, from cdn.NetBSD.org."),
    ("DER/32 0.1 “Genesis” boots", "index.html#news", "Thu, 19 Nov 2026 18:46:00 -0700",
     "The first development release builds on Arch Linux and boots to a login prompt as a disk image and "
     "as a live ISO."),
]
items = "\n".join(
    f"  <item>\n    <title>{html.escape(t)}</title>\n    <link>{URL}{l}</link>\n"
    f"    <guid isPermaLink=\"false\">der32-{i}</guid>\n    <pubDate>{d}</pubDate>\n"
    f"    <description>{html.escape(x)}</description>\n  </item>"
    for i, (t, l, d, x) in zip(range(len(NEWS), 0, -1), NEWS))
open(os.path.join(SITE, "rss.xml"), "w").write(f"""<?xml version="1.0" encoding="UTF-8"?>
<rss version="2.0">
<channel>
  <title>DER/32 news</title>
  <link>{URL}</link>
  <description>News from the DER/32 project, a NetBSD-derived UNIX.</description>
  <language>en</language>
{items}
</channel>
</rss>
""")
print("wrote site/rss.xml")
