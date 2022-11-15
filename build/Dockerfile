FROM debian:bookworm-slim

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && \
	apt-get install -y libsqlite3-dev cmake libiodbc2-dev libcpprest-dev libboost-dev g++ git zlib1g-dev && \
	apt-get install -y openssl curl nlohmann-json3-dev libcurl4-openssl-dev

RUN apt-get install python3-pip --yes
RUN pip3 install --upgrade pip && \
  apt-get update && pip3 install conan

RUN conan profile new default --detect && \
	conan profile update settings.compiler.cppstd=17 default

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
	&& cd deps \
	&& conan create local_export/geohashconan.py \
	&& conan create local_export/sqlite_modern_cpp_conan.py \
	&& conan install . --build=missing

RUN cd epanet-rtx/build/cmake \
	&& mkdir build && cd build \
	&& cmake -DCMAKE_BUILD_TYPE=Release .. \
	&& make install -j8 \
	&& apt-get install -y libboost-iostreams-dev libboost-program-options-dev

WORKDIR /opt/src/epanet-rtx

RUN cd examples/LINK/service \
	&& mkdir build && cd build \
	&& cmake -DCMAKE_BUILD_TYPE=Release .. \
	&& make install -j8
