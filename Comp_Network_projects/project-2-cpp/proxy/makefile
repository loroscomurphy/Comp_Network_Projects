a.out: proxy.cpp proxy.h ../common/CommandHandler.cpp ../common/CommandHandler.h
	g++ proxy.cpp ../common/CommandHandler.cpp -o a.out

debug: proxy.cpp proxy.h ../common/CommandHandler.cpp ../common/CommandHandler.h
	g++ -g -DDEBUG proxy.cpp ../common/CommandHandler.cpp -o a.out

run: a.out
	./a.out

run-debug: debug
	./a.out

clean:
	rm a.out
	rm *.o