#!/bin/bash
# Test MCP server functionality
# Usage: ./scripts/test-mcp.sh

set -e

GRIDEX_BIN="${1:-.build/debug/Gridex}"

echo "Testing Gridex MCP Server..."
echo "Binary: $GRIDEX_BIN"
echo ""

# Create a named pipe for communication
PIPE_IN=$(mktemp -u)
PIPE_OUT=$(mktemp -u)
mkfifo "$PIPE_IN"
mkfifo "$PIPE_OUT"

# Cleanup on exit
cleanup() {
    rm -f "$PIPE_IN" "$PIPE_OUT"
    [ -n "$MCP_PID" ] && kill "$MCP_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Start MCP server in background
"$GRIDEX_BIN" --mcp-stdio < "$PIPE_IN" > "$PIPE_OUT" 2>/dev/null &
MCP_PID=$!

# Give it a moment to start
sleep 1

# Check if process is running
if ! kill -0 "$MCP_PID" 2>/dev/null; then
    echo "ERROR: MCP server failed to start"
    exit 1
fi

echo "MCP server started (PID: $MCP_PID)"

# Send initialize request
echo "Sending initialize request..."
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"clientInfo":{"name":"test-client","version":"1.0.0"}}}' > "$PIPE_IN" &

# Read response with timeout
RESPONSE=$(timeout 5 head -n 1 "$PIPE_OUT" 2>/dev/null || echo "TIMEOUT")

if [ "$RESPONSE" = "TIMEOUT" ]; then
    echo "ERROR: No response from MCP server (timeout)"
    exit 1
fi

echo "Response: $RESPONSE"

# Check for success
if echo "$RESPONSE" | grep -q '"result"'; then
    echo ""
    echo "SUCCESS: MCP server is responding correctly!"
    echo ""
    echo "You can now configure your AI client with:"
    echo ""
    echo '{
  "mcpServers": {
    "gridex": {
      "command": "'$(realpath "$GRIDEX_BIN")'",
      "args": ["--mcp-stdio"]
    }
  }
}'
else
    echo "ERROR: Unexpected response"
    exit 1
fi
