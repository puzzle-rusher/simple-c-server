FROM alpine:latest

RUN apk update && apk upgrade
RUN apk add build-base
RUN apk add cmake
#RUN apk add strace
#RUN apk add busybox-extras
#RUN apk add gdb
WORKDIR /program
COPY . .
RUN cmake -DCMAKE_BUILD_TYPE=Debug .
RUN make
EXPOSE 80

ENTRYPOINT ["./final", "-p", "80", "-d", "./files", "-h", "0.0.0.0"]
