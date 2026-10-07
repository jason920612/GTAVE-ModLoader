// relocate: finds things in the game's code and follows them from one game build to the next (research/phase0.md §27).
//
// Images are dumps of the running game (tools/dump.py) or plain executables. Addresses are RVAs (hex).
//
//   relocate info    <image>                      functions, strings, sections
//   relocate func    <image> <rva>                the function containing rva: bounds, callers, callees, strings, code
//   relocate strings <image> <text>               strings containing text and the functions using them
//   relocate xrefs   <image> <rva>                code referencing rva (calls, jumps, data)
//   relocate pattern <image> "<bytes>"            matches of a pattern ("48 8B 05 ? ? ? ?")
//   relocate sig     <image> <rva>                a unique pattern for rva
//   relocate match   <old> <new> <rva|file> ...   rva in the new build, with a pattern; a file lists "name rva" lines
//   relocate stats   <old> <new>                  how many functions were matched, by method
//   relocate eval    <old> <new>                  accuracy: half the string anchors are left out and looked for
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "analysis.hpp"
#include "match.hpp"
#include "signature.hpp"

using namespace relocate;

namespace
{
	uint32_t Hex(const char* s)
	{
		return static_cast<uint32_t>(std::stoull(s, nullptr, 16));
	}

	bool Load(Index& index, const char* path)
	{
		const auto start = std::chrono::steady_clock::now();
		std::string error;
		if (!index.Build(path, error))
		{
			fprintf(stderr, "%s: %s\n", path, error.c_str());
			return false;
		}
		fprintf(stderr, "%s: %zu functions, %zu strings (%lld ms)\n", path, index.Functions().size(), index.Strings().size(),
		    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()));
		return true;
	}

	void PrintFunction(const Index& index, int f, bool code)
	{
		const FunctionInfo& info = index.Functions()[f];
		printf("function %#x-%#x, %u instructions\n", info.begin, info.end, info.instructions);
		printf("  callers:");
		for (const int c : info.callers)
			printf(" %#x", index.Functions()[c].begin);
		printf("\n  callees:");
		for (const uint32_t c : info.callees)
			printf(" %#x", c);
		printf("\n");
		for (const int s : info.strings)
			printf("  string \"%s\"\n", index.Strings()[s].c_str());
		if (code)
			for (const auto& i : Decode(index.Img(), info.begin, info.end))
				printf("  %#09x  %s\n", i.rva, Format(index.Img(), i.rva).c_str());
	}

	std::vector<std::pair<std::string, uint32_t>> Targets(int argc, char** argv, int first)
	{
		std::vector<std::pair<std::string, uint32_t>> out;
		for (int i = first; i < argc; ++i)
		{
			std::ifstream list(argv[i]);
			if (!list)
			{
				out.emplace_back(argv[i], Hex(argv[i]));
				continue;
			}
			for (std::string line; std::getline(list, line);)
			{
				std::istringstream in(line);
				std::string name, rva;
				if (line.empty() || line[0] == '#' || !(in >> name >> rva))
					continue;
				out.emplace_back(name, static_cast<uint32_t>(std::stoull(rva, nullptr, 16)));
			}
		}
		return out;
	}
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		fprintf(stderr, "usage: relocate info|func|strings|xrefs|pattern|sig|match|stats ... (see the top of main.cpp)\n");
		return 1;
	}
	const std::string command = argv[1];
	Index a;
	if (!Load(a, argv[2]))
		return 1;

	if (command == "info")
	{
		printf("base %#llx\n", static_cast<unsigned long long>(a.Img().Base()));
		for (const auto& s : a.Img().Sections())
			printf("  %-8s %#09x size %#x%s\n", s.name.c_str(), s.rva, s.size, s.code ? " code" : "");
		return 0;
	}
	if (command == "func" && argc >= 4)
	{
		const int f = a.Img().FunctionOf(Hex(argv[3]));
		if (f < 0)
			return printf("not inside a function\n"), 1;
		PrintFunction(a, f, true);
		return 0;
	}
	if (command == "strings" && argc >= 4)
	{
		for (size_t s = 0; s < a.Strings().size(); ++s)
			if (a.Strings()[s].find(argv[3]) != std::string::npos)
			{
				printf("\"%s\":", a.Strings()[s].c_str());
				for (const int f : a.StringUsers(static_cast<int>(s)))
					printf(" %#x", a.Functions()[f].begin);
				printf("\n");
			}
		return 0;
	}
	if (command == "xrefs" && argc >= 4)
	{
		const uint32_t target = Hex(argv[3]);
		for (const auto& f : a.Functions())
		{
			const bool calls = std::find(f.callees.begin(), f.callees.end(), target) != f.callees.end();
			const bool data = std::find(f.dataRefs.begin(), f.dataRefs.end(), target) != f.dataRefs.end();
			if (!calls && !data)
				continue;
			for (const auto& i : Decode(a.Img(), f.begin, f.end))
				if (i.ref != Ref::None && i.target == target)
					printf("%#09x (function %#x)  %s\n", i.rva, f.begin, Format(a.Img(), i.rva).c_str());
		}
		return 0;
	}
	if (command == "pattern" && argc >= 4)
	{
		const auto p = Pattern::Parse(argv[3]);
		if (!p)
			return fprintf(stderr, "bad pattern\n"), 1;
		for (const uint32_t at : Find(a.Img(), *p, 64))
		{
			const int f = a.Img().FunctionOf(at);
			printf("%#09x  function %#x  %s\n", at, f >= 0 ? a.Functions()[f].begin : 0, Format(a.Img(), at).c_str());
		}
		return 0;
	}
	if (command == "sig" && argc >= 4)
	{
		const std::string sig = MakeSignature(a.Img(), Hex(argv[3]));
		printf("%s\n", sig.empty() ? "(no unique pattern)" : sig.c_str());
		return sig.empty();
	}
	if (command == "eval" && argc >= 4)
	{
		Index b;
		if (!Load(b, argv[3]))
			return 1;
		Matcher m(a, b, true);
		const auto e = m.Evaluate();
		printf("left-out anchors: %d found correctly, %d wrongly, %d not found (matched %zu functions)\n", e.correct, e.wrong, e.missing,
		    m.Count(How::String) + m.Count(How::Exact) + m.Count(How::Graph));
		return 0;
	}
	if ((command == "match" || command == "stats") && argc >= 4)
	{
		Index b;
		if (!Load(b, argv[3]))
			return 1;
		const auto start = std::chrono::steady_clock::now();
		Matcher m(a, b);
		fprintf(stderr, "matched %zu of %zu functions (string %zu, exact %zu, call graph %zu) in %lld ms\n",
		    m.Count(How::String) + m.Count(How::Exact) + m.Count(How::Graph), a.Functions().size(), m.Count(How::String), m.Count(How::Exact),
		    m.Count(How::Graph),
		    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()));
		if (command == "stats")
			return 0;
		int failed = 0;
		for (const auto& [name, rva] : Targets(argc, argv, 4))
		{
			const auto r = m.Relocate(rva);
			if (!r.ok)
			{
				++failed;
				printf("%-32s %#09x -> not found (%s)\n", name.c_str(), rva, r.note.c_str());
				continue;
			}
			const std::string sig = b.Img().IsCode(r.rva) ? MakeSignature(b.Img(), r.rva) : std::string();
			printf("%-32s %#09x -> %#09x  [%s] %s\n", name.c_str(), rva, r.rva, Name(r.how), r.note.c_str());
			if (!sig.empty())
				printf("%-32s   pattern %s\n", "", sig.c_str());
		}
		return failed ? 2 : 0;
	}
	fprintf(stderr, "unknown command or missing arguments\n");
	return 1;
}
