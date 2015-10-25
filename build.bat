@echo off
for %%* in (.) do set ProjectName=%%~n*
echo %ProjectName%
make
arm-none-eabi-strip %ProjectName%.elf
res\bannertool.exe makebanner -i res\banner.png -a res\audio.wav -o res\banner.bnr
res\bannertool.exe makesmdh -s "FTP-3DS" -l "FTP-3DS" -p "mtheall & iamevn" -i icon.png  -o res\icon.icn
res\makerom.exe -f cci -rsf res\gw_workaround.rsf -target d -exefslogo -elf %ProjectName%.elf -icon res\icon.icn -banner res\banner.bnr -o %ProjectName%.3ds
res\makerom.exe -f cia -o %ProjectName%.cia -elf %ProjectName%.elf -rsf res\build_cia.rsf -icon res\icon.icn -banner res\banner.bnr -exefslogo -target t
echo Done!
pause