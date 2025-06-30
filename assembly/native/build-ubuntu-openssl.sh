mkdir -p /tmp/3pp || echo "3pp exists"

if [ ! -d "/tmp/3pp/openssl_3" ]; then
  git clone https://github.com/openssl/openssl /tmp/3pp/openssl_3
  cd /tmp/3pp/openssl_3

  export OPENSSL_PATH=`pwd`

  git checkout openssl-3.1.4
  ./config

  make build_libs -j$(nproc)
  test $? -eq 0 || { echo "Can't compile openssl_3"; exit 1; }
else
  export OPENSSL_PATH=/tmp/3pp/openssl_3
  echo "Using compiled openssl_3"
fi