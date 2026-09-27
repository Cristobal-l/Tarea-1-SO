CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17

planificador: planificador.cpp
	$(CXX) $(CXXFLAGS) planificador.cpp -o planificador -lpthread

clean:
	rm -f planificador
