FROM debian:stable-slim

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && \
    apt-get install -y libsqlite3-dev cmake libiodbc2-dev libcpprest-dev libboost-dev g++ git llvm clang zlib1g-dev && \
    apt-get install -y openssl curl nlohmann-json3-dev

RUN apt-get install python3-pip --yes
RUN pip3 install --upgrade pip && \
    apt-get update && pip3 install conan

RUN conan profile new default --detect && \
    conan profile update settings.compiler=clang default && \
    conan profile update settings.compiler.version=11 default && \
    conan profile update settings.compiler.libcxx=libstdc++11 default && \
    conan profile update env.CC=/usr/bin/clang default && \
    conan profile update env.CXX=/usr/bin/clang++ default

WORKDIR /opt/src

## build/install EPANET
RUN git clone https://github.com/09c42c/EPANET.git \
	&& cd EPANET \
	&& git checkout no-conan \
	&& mkdir build && cd build \
	&& cmake .. \
	&& make install

##########################################################################################
####  everything above here should be pretty stable. cache this.
##########################################################################################

RUN git clone https://github.com/oatpp/oatpp.git \
	&& cd oatpp \
	&& mkdir build && cd build \
	&& cmake .. \
	&& make install \
	&& cd ../.. \ 
	&& git clone https://github.com/oatpp/oatpp-openssl.git \
	&& cd oatpp-openssl \
	&& mkdir build && cd build \
	&& cmake .. \
	&& make install \
	&& cd ../.. \
	&& rm -rf oatpp \
	&& rm -rf oatpp-openssl

RUN git clone https://github.com/09c42c/epanet-rtx.git \
	&& cd epanet-rtx \
	&& git checkout dev \
	&& cd deps \
	&& conan create local_export/geohashconan.py \
	&& conan create local_export/sqlite_modern_cpp_conan.py \
	&& conan install . --build=missing

RUN cd epanet-rtx/build/cmake \
	&& mkdir build && cd build \
	&& cmake -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ .. \
	&& make \
	#&& make test ARGS="-V" \
	&& make install
