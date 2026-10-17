#!/usr/bin/env python3
# Recurrence guard for spec 12 (HLE work-buffer trust-boundary hardening).
#
# Structs that are placed in guest memory (allocated by a guest malloc callback, by vm::alloc, or
# constructed at a vm address) must contain only ABI-visible, guest-writable data: be_t<> fields,
# fixed-width integers, vm:: pointers (which are 32-bit guest addresses and stay inside the guest
# reservation when dereferenced), opaque u32 IDs, nested ABI structs and explicit padding.
#
# They must NOT persist trusted host state, because the guest can overwrite every byte at any time.
# The escape-capable patterns this guard looks for are:
#   - native pointers (Type*), which a guest can rewrite to any host address;
#   - host-sized integers (usz / size_t / uintptr_t), used as offsets added to host pointers;
#   - native-endian 64-bit integers (u64 / s64 not wrapped in be_t/le_t), which are not guest ABI data
#     and can carry sizes or offsets beyond the 32-bit guest address space;
#   - owning host containers and smart pointers (std::vector, std::unique_ptr, stx::shared_ptr, ...).
#
# This is a heuristic line scanner, NOT a parser. It cannot prove the boundary is safe: it does not
# resolve typedefs, templates or nested types it has not been told about, and it only inspects the
# struct types listed in GUEST_RESIDENT below. It exists to catch an accidental reintroduction of a
# native pointer into one of the known guest-resident structs, and must be paired with human review
# for anything new. Every allowlist entry documents why the flagged member is safe.
#
# Run `python hle_guest_struct_guard.py --self-test` for the regression tests, or with no arguments
# to scan the modules (pass a repo root as the first argument if not run from rpcs3/tests).

import os
import re
import sys

# Guest-resident structs to scan, keyed by the header they are defined in (relative to Emu/Cell/Modules).
GUEST_RESIDENT = {
	"cellAtracXdec.h": ["AtracXdecDecoder", "AtracXdecContext"],
	"cellAdec.h": ["LpcmDecContext", "AdecContext", "AdecFrame"],
	"cellDmuxPamf.h": ["DmuxPamfContext", "DmuxPamfElementaryStream", "DmuxPamfQueueReserved"],
	"cellPngDec.h": ["PngHandle", "PngStream", "PngBuffer"],
	"cellFont.h": ["CellFont"],
	"cellSail.h": ["CellSailRendererVideoAttribute", "CellSailRendererAudioAttribute"],
}

# (struct, member) -> reason it is safe despite matching a forbidden pattern.
# Every guest-resident struct was reworked to hold only IDs, ABI data and padding; the entries below are native
# 64-bit values that never size, offset or select a host access.
# Add an entry here only with a review note explaining why the member cannot select host state.
ALLOWLIST = {
	("AtracXdecContext", "cmd_counter"): "debug counter, only incremented and printed in trace logs",
	("LpcmDecContext", "cmd_counter"): "debug counter, only incremented and printed in trace logs",
	("DmuxPamfContext", "au_queue_full_bitset"): "bit set of full AU queues, only bit-tested; bits are set with an es_index bounded below 64 (T-12.4)",
}

FORBIDDEN = [
	# A native pointer member: a type token, then * and a member name at the end of the statement.
	# vm:: pointers are excluded separately (they are 32-bit guest addresses).
	(re.compile(r"[A-Za-z_][\w:<>,]*\s*\*\s*[A-Za-z_]\w*$"), "native pointer"),
	(re.compile(r"\b(usz|size_t|uintptr_t|intptr_t|std::size_t)\b"), "host-sized integer"),
	(re.compile(r"\b(u64|s64|std::u?int64_t|u?int64_t)\b"), "native 64-bit integer"),
	(re.compile(r"\b(std::(vector|string|unique_ptr|shared_ptr|map|unordered_map|deque|list|function|span|optional)|stx::shared_ptr)\b"), "owning host container"),
]

def struct_body(text, name):
	# Returns the text between the outermost braces of `struct/class name { ... }`, or None.
	m = re.search(r"\b(?:struct|class)\s+(?:alignas\s*\([^)]*\)\s+)?" + re.escape(name) + r"\b[^\{;]*\{", text)
	if not m:
		return None
	depth = 0
	start = m.end() - 1
	for i in range(start, len(text)):
		if text[i] == "{":
			depth += 1
		elif text[i] == "}":
			depth -= 1
			if depth == 0:
				return text[start + 1:i]
	return None

def member_name(line):
	# The member name is the last identifier before an array subscript or initializer.
	decl = line.split("=", 1)[0].split("[", 1)[0]
	ids = re.findall(r"[A-Za-z_]\w*", decl)
	return ids[-1] if ids else ""

def scan_struct(name, body):
	findings = []
	# Only look at the struct's own data members, not nested type definitions or method bodies.
	depth = 0
	for raw in body.splitlines():
		# Strip line comments.
		code = raw.split("//", 1)[0]
		# Strip brace initializers ("u64 size{};", "T a{1, 2};") so they aren't mistaken for a method body.
		code = re.sub(r"([\w\]])\s*\{[^{}]*\}(?=\s*[;,])", r"\1", code)
		opens = code.count("{")
		closes = code.count("}")
		if depth > 0:
			depth += opens - closes
			continue
		depth += opens - closes
		# A method body or nested type opened on this line: inspect nothing after the brace.
		code = code.split("{", 1)[0]
		for stmt in code.split(";"):
			line = stmt.strip()
			if not line:
				continue
			if line.startswith(("static", "virtual", "friend", "using", "typedef", "enum", "public", "private", "protected", "return", "#")):
				continue
			if "(" in line:  # function declaration or call
				continue
			# Drop an initializer so a pointer member ends at its name (e.g. "T* p = nullptr").
			decl = line.split("=", 1)[0].strip()
			for pattern, reason in FORBIDDEN:
				if reason == "native pointer":
					if "vm::" in decl or not pattern.search(decl):
						continue
				elif reason == "native 64-bit integer":
					# be_t<u64> / le_t<u64> are fixed-endian ABI fields, and so is the address type of a vm:: pointer
					if not pattern.search(re.sub(r"\b(be_t|le_t|vm::\w+)\s*<[^;]*>", "", decl)):
						continue
				elif not pattern.search(line):
					continue
				if (name, member_name(line)) in ALLOWLIST:
					break
				findings.append((name, line, reason))
				break
	return findings

def scan_text(name, text):
	body = struct_body(text, name)
	if body is None:
		return [(name, "<struct not found>", "missing")]
	return scan_struct(name, body)

def scan_repo(root):
	modules = os.path.join(root, "rpcs3", "Emu", "Cell", "Modules")
	if not os.path.isdir(modules):
		modules = os.path.join(root, "Emu", "Cell", "Modules")
	findings = []
	for header, names in GUEST_RESIDENT.items():
		path = os.path.join(modules, header)
		with open(path, encoding="utf-8") as f:
			text = f.read()
		for name in names:
			findings += scan_text(name, text)
	return findings

SELF_TEST_CASES = [
	# (struct text, expected finding count)
	("struct Ok { be_t<u32> a; vm::bptr<u8> p; u8 pad[4]; be_t<u32> host_id; };", 0),
	("struct Bad1 { be_t<u32> a; AVCodecContext* ctx; };", 1),
	("struct Bad2 { usz cursor; be_t<u32> a; };", 1),
	("struct Bad3 { std::vector<u8> data; };", 1),
	("struct Bad4 { be_t<u32> a; ppu_thread* ppu = nullptr; vm::ptr<void> q; };", 1),
	# vm:: pointers and nested ABI arrays are fine.
	("struct Ok2 { vm::bcptr<u8, u64> addr; be_t<u64> size; std::array<u8, 0x10> info; };", 0),
	# The pre-hardening guest-resident GifStream: a native u64 size drove a raw copy from guest memory.
	("struct Bad5 { u32 fd{}; u64 fileSize{}; CellGifDecInfo info{}; };", 1),
	("struct Ok3 { be_t<u64> userData; le_t<s64> pts; be_t<u32> host_id; };", 0),
]

def self_test():
	ok = True
	for i, (text, expected) in enumerate(SELF_TEST_CASES):
		name = re.search(r"struct\s+(\w+)", text).group(1)
		got = len(scan_struct(name, struct_body(text, name)))
		status = "ok" if got == expected else "FAIL"
		if got != expected:
			ok = False
		print(f"  case {i} ({name}): expected {expected}, got {got} [{status}]")
	# The allowlist mechanism must suppress exactly the listed (struct, member).
	sample = "struct Sample { ppu_thread* ppu; AVCodecContext* ctx; };"
	ALLOWLIST[("Sample", "ppu")] = "self-test"
	try:
		got = scan_struct("Sample", struct_body(sample, "Sample"))
	finally:
		del ALLOWLIST[("Sample", "ppu")]
	if len(got) == 1 and got[0][0] == "Sample" and member_name(got[0][1]) == "ctx":
		print("  allowlist case: ok")
	else:
		print(f"  allowlist case: FAIL ({got})")
		ok = False
	print("self-test passed" if ok else "self-test FAILED")
	return 0 if ok else 1

def main():
	args = sys.argv[1:]
	if "--self-test" in args:
		return self_test()
	root = args[0] if args else os.path.join(os.path.dirname(__file__), "..", "..")
	findings = scan_repo(os.path.abspath(root))
	for name, line, reason in findings:
		print(f"{name}: {reason}: {line}")
	if findings:
		print(f"{len(findings)} finding(s): a guest-resident struct holds trusted host state")
		return 1
	print("ok: no guest-resident struct holds a native pointer, host-sized or native 64-bit integer, or host container")
	return 0

if __name__ == "__main__":
	sys.exit(main())
