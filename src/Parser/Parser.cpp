#include "Parser.hpp"
#include <iostream>

inline DIA active_arch()
{
    const static DIA arch = run_diagnostics();
    return arch;
}

void init()
{
    switch (active_arch())
    {
    case DIA::NONE:
        std::cerr << "Error during init\n";
        break;
    case DIA::AVX512:
        break;
    case DIA::AVX2:
        break;
    case DIA::SSE4_2:
        break;
    case DIA::NEON:
        break;
    default:
        break;
    }
}