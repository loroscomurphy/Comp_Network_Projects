proxy_http.out : proxy_http.cpp proxy_http.h ../common/CommandHandler.cpp ../common/CommandHandler.h
	g++ proxy_http.cpp ../common/CommandHandler.cpp -o proxy_http.out

debug: proxy_http.cpp proxy_http.h ../common/CommandHandler.cpp ../common/CommandHandler.h
	g++ -g -DDEBUG proxy_http.cpp ../common/CommandHandler.cpp -o proxy_http.out

run: proxy_http.out
	./proxy_http.out

run-debug: debug
	./proxy_http.out

clean:
	rm proxy_http.out