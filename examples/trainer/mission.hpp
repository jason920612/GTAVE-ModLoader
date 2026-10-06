#pragma once

// Passing the running story mission through its own code (research/phase0.md §22).
namespace mission
{
	// Finds the running mission script, optionally sets every visible mission stat to a passing value (gold
	// medal), and makes the script run its own "mission passed" routine. Script fiber only.
	void Pass(bool gold);

	// Every frame: keeps the stats at their passing values until the results screen took them.
	void Frame();
}
