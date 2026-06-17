/* Export the TeX Live Windows DLL entrypoint for the standalone MinGW build. */

extern int main(int ac, char **av);

__declspec(dllexport) int
dllxetexmain(int ac, char **av)
{
  return main(ac, av);
}
