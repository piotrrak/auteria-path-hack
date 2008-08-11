MY_CXXFLAGS = $(CXXFLAGS) -fPIC -fvisibility=hidden -Wall -Wextra -Wshadow $(shell pcre-config --cflags)
MY_LDFLAGS = -shared -fPIC $(shell pcre-config --libs)

objects = libAPH.o

all: libAPH.so

libAPH.so: $(objects)
	$(CXX) -o $@ $(MY_LDFLAGS) $< 

$(objects): %.o: %.cc
	$(CXX) -o $@ -c $< $(MY_CXXFLAGS) 

.PHONY: clean

clean:
	rm -f *.o *.so

