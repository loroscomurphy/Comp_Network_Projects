a.out: client.cpp client.h ../common/CommandHandler.cpp ../common/CommandHandler.h
	g++ client.cpp ../common/CommandHandler.cpp -o a.out

debug: client.cpp client.h ../common/CommandHandler.cpp ../common/CommandHandler.h
	g++ -g -DDEBUG client.cpp ../common/CommandHandler.cpp -o a.out

run: a.out
	./a.out localhost

run-debug: debug
	./a.out localhost

clean: 
	rm a.out
	rm *.o

# exClient: exClient.cpp
# 	g++ exClient.cpp -o exClient
# exServer: exServer.cpp
# 	g++ exServer.cpp -o exServer
# run: exClient exServer
# 	./exClient localhost
# clean:
# 	rm exClient
# 	rm exServer
# 	rm *.o
