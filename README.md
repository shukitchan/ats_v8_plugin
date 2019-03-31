ATS V8 Plugin
====
 - Follow instructions here to install v8 - https://v8.dev/docs/embed
 - Make sure you have ATS latest master branch installed 
 - To compile run this - tsxs -v -I$HOME/v8/v8/include -lv8_monolith -L$HOME/v8/v8/out.gn/x64.release.sample/obj/ -o v8.so v8.cc

