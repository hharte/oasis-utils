dos2unix *.c *.h
uncrustify -c uncrustify.cfg --replace -l c *.c *.h
rm *.unc-backup*
cpplint --linelength=400 --filter=-readability/casting *.h *.c
cppcheck *.c *.h
clang-tidy *.c *.h
