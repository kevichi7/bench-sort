# Multi-stage build: build C++ core + Go API with cgo
FROM debian:stable-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential g++ make git \
    libtbb-dev \
    golang && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY . .
RUN make && make test
WORKDIR /app/api/go
ENV CGO_ENABLED=1
RUN SORTBENCH_CGO=1 go build -o /out/sortbench-api .

FROM debian:stable-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libtbb2 libgomp1 && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/sortbench-api /usr/local/bin/sortbench-api
EXPOSE 8080
ENV PORT=8080
ENV SORTBENCH_CGO=1
ENTRYPOINT ["/usr/local/bin/sortbench-api"]

