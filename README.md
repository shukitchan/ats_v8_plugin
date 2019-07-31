ATS V8 Plugin
====
 - Follow instructions here to install v8 - https://v8.dev/docs/embed
 - Make sure you have ATS latest master branch installed 
 - To compile run this - tsxs -v -I$HOME/v8/v8/include -lv8_monolith -L$HOME/v8/v8/out.gn/x64.release.sample/obj/ -o v8.so v8.cc
 - Copy v8.so to /usr/local/libexec/trafficserver/
 - Copy test.js in this repo to /usr/local/var/js/
 - Add remap rules to /usr/local/etc/trafficserver/remap.config to use the plugin. Pass in the test.js as parameter. 
 - E.g. map http://test.com/ http://httpbin.org/ @plugin=v8.so @pparam=/usr/local/var/js/test.js 
