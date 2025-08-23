#!/bin/bash

urls=(
  "https://localhost:4221/results"
  "https://localhost:4221/results"
  "https://localhost:4221/results"
  "https://localhost:4221/agents"
  "https://localhost:4221/agents"
  "https://localhost:4221/results"
  "https://localhost:4221/results"
  "https://localhost:4221/results"
  "https://localhost:4221/results"
  "https://localhost:4221/results"
  "https://localhost:4221/results"
  "https://localhost:4221/results"
  "https://localhost:4221/results"
  "https://localhost:4221/results"
  "https://localhost:4221/agents"
  "https://localhost:4221/agents"
  "https://localhost:4221/agents"
  "https://localhost:4221/results"
  "https://localhost:4221/agents"
  "https://localhost:4221/agents"
  "https://localhost:4221/results"
  "https://localhost:4221/results"
  "https://localhost:4221/results"
  "https://localhost:4221/results"
  "https://localhost:4221/results"
  "https://localhost:4221/results"
  "https://localhost:4221/results"
  "https://localhost:4221/results"
  "https://localhost:4221/results"
  "https://localhost:4221/agents"
  "https://localhost:4221/agents"
)


tmpfiles=()

for i in "${!urls[@]}"; do
  tmpfile=$(mktemp)
  errfile=$(mktemp)
  tmpfiles[$i]=$tmpfile

  (
    sleep 1
    http_code=$(curl --cacert ../certs/server-cert.pem \
                     -s -o /dev/null \
                     -S -w "%{http_code}" \
                     "${urls[$i]}" 2>"$errfile")
    exit_code=$?

    {
      printf "\n\n=== Request %d ===\n" "$((i+1))"
      if [ $exit_code -eq 0 ]; then
        echo "Success: HTTP $http_code"
      else
        echo "Curl error code: $exit_code"
        echo "HTTP status (if any): $http_code"
        echo "Error string:"
        cat "$errfile"
      fi
    } >"$tmpfile"

    rm -f "$errfile"
  ) &
done

wait

#printing results
for i in "${!urls[@]}"; do
  cat "${tmpfiles[$i]}"
  rm -f "${tmpfiles[$i]}"
done
