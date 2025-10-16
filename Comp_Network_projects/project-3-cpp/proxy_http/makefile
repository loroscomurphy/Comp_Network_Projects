a.out : proxy_http.cpp proxy_http.h ../common/CommandHandler.cpp ../common/CommandHandler.h
	g++ proxy_http.cpp ../common/CommandHandler.cpp -o a.out

debug: proxy_http.cpp proxy_http.h ../common/CommandHandler.cpp ../common/CommandHandler.h
	g++ -g -DDEBUG proxy_http.cpp ../common/CommandHandler.cpp -o a.out

run: a.out
	./a.out

run-debug: debug
	./a.out

clean:
	rm a.out
	rm *.o