@echo off
for %%* in (.) do set ProjectName=%%~n*
echo %ProjectName%
make
arm-none-eabi-strip %ProjectName%.elf
res\makerom.exe -f cci -rsf res\gw_workaround.rsf -target d -exefslogo -elf %ProjectName%.elf -icon res\icon.bin -banner res\banner.bin -o %ProjectName%.3ds
res\makerom.exe -f cia -o %ProjectName%.cia -elf %ProjectName%.elf -rsf res\build_cia.rsf -icon res\icon.bin -banner res\banner.bin -exefslogo -target t
echo Done!
pause