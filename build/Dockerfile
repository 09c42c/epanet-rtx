FROM debian:bookworm-slim

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && \
	apt-get install -y libsqlite3-dev cmake libiodbc2-dev libcpprest-dev libboost-dev g++ git zlib1g-dev && \
	apt-get install -y openssl curl nlohmann-json3-dev libcurl4-openssl-dev

WORKDIR /opt/src

RUN git clone https://github.com/09c42c/oatpp.git \
	&& cd oatpp \
	&& mkdir build && cd build \
	&& cmake -DCMAKE_BUILD_TYPE=Release -DOATPP_BUILD_TESTS=OFF .. \
	&& make install -j8 \
	&& cd ../.. \
	&& git clone https://github.com/09c42c/oatpp-openssl.git \
	&& cd oatpp-openssl \
	&& mkdir build && cd build \
	&& cmake -DCMAKE_BUILD_TYPE=Release -DOATPP_BUILD_TESTS=OFF .. \
	&& make install -j8 \
	&& cd ../.. \
	&& rm -rf oatpp \
	&& rm -rf oatpp-openssl

RUN git clone https://github.com/simplegeo/libgeohash.git \
	&& cd libgeohash \
	&& gcc -fPIC -c geohash.c -o geohash.o \
	&& ar rcs libgeohash.a geohash.o \
	&& cp geohash.h /usr/local/include \
	&& cp libgeohash.a /usr/local/lib \
	&& cd .. \
	&& rm -rf libgeohash

RUN git clone https://github.com/SqliteModernCpp/sqlite_modern_cpp.git \
	&& cp -R sqlite_modern_cpp/hdr/* /usr/local/include \
	&& rm -rf sqlite_modern_cpp

# ##########################################################################################
# ####  everything above here should be pretty stable. cache this.
# ##########################################################################################

## build/install EPANET
RUN git clone https://github.com/09c42c/EPANET.git \
	&& cd EPANET \
	&& mkdir build && cd build \
	&& cmake -DCMAKE_BUILD_TYPE=Release .. \
	&& make install -j8 \
	&& cd ../.. \
	&& rm -rf EPANET

RUN git clone https://github.com/09c42c/epanet-rtx.git \
	&& cd epanet-rtx \
	&& git checkout dev \
	&& cd build/cmake \
	&& mkdir build && cd build \
	&& cmake -DCMAKE_BUILD_TYPE=Release .. \
	&& make install -j8 \
	&& apt-get install -y libboost-iostreams-dev libboost-program-options-dev

WORKDIR /opt/src/epanet-rtx

RUN cd examples/LINK/service \
	&& mkdir build && cd build \
	&& cmake -DCMAKE_BUILD_TYPE=Release .. \
	&& make install -j8

RUN curl -fsSL https://deb.nodesource.com/setup_18.x | bash - \
	&& apt-get install -y nodejs
