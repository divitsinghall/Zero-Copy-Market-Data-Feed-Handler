#!/bin/bash
#
# profile_benchmark.sh - Cross-platform profiling for ITCH parser benchmarks
#
# Usage: ./scripts/profile_benchmark.sh [benchmark_filter]
#
# Examples:
#   ./scripts/profile_benchmark.sh                    # Profile all benchmarks
#   ./scripts/profile_benchmark.sh "ZeroCopy"         # Profile zero-copy only
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"
BENCHMARK_BIN="$BUILD_DIR/itch_benchmark"
OUTPUT_DIR="$PROJECT_ROOT/profiling_results"

# Benchmark filter (default: all ITCH benchmarks)
FILTER="${1:-ITCH|Single|Raw}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
echo_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
echo_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# Ensure benchmark binary exists
if [ ! -f "$BENCHMARK_BIN" ]; then
    echo_error "Benchmark binary not found: $BENCHMARK_BIN"
    echo_info "Building benchmarks..."
    cmake -B "$BUILD_DIR" -S "$PROJECT_ROOT" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD_DIR" --target itch_benchmark -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
fi

# Create output directory
mkdir -p "$OUTPUT_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Detect OS and run appropriate profiler
case "$(uname -s)" in
    Linux*)
        echo_info "Detected Linux - using perf"
        
        # Check if perf is available
        if ! command -v perf &> /dev/null; then
            echo_error "perf not found. Install with: sudo apt install linux-tools-common linux-tools-$(uname -r)"
            exit 1
        fi
        
        # Check perf permissions
        if [ ! -r /proc/sys/kernel/perf_event_paranoid ]; then
            echo_warn "Cannot read perf_event_paranoid. May need sudo."
        else
            PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid)
            if [ "$PARANOID" -gt 2 ]; then
                echo_warn "perf_event_paranoid=$PARANOID. For full profiling, run:"
                echo "  sudo sysctl -w kernel.perf_event_paranoid=-1"
            fi
        fi
        
        PERF_DATA="$OUTPUT_DIR/perf_${TIMESTAMP}.data"
        
        echo_info "Recording with perf (filter: $FILTER)..."
        perf record -g -o "$PERF_DATA" \
            "$BENCHMARK_BIN" \
            --benchmark_filter="$FILTER" \
            --benchmark_min_time=2
        
        echo_info "Generating perf report..."
        perf report -i "$PERF_DATA" --stdio > "$OUTPUT_DIR/perf_report_${TIMESTAMP}.txt"
        
        echo_info "Generating flame graph data..."
        perf script -i "$PERF_DATA" > "$OUTPUT_DIR/perf_script_${TIMESTAMP}.txt"
        
        echo ""
        echo_info "Results saved to:"
        echo "  - $PERF_DATA"
        echo "  - $OUTPUT_DIR/perf_report_${TIMESTAMP}.txt"
        echo ""
        echo_info "View interactive report with: perf report -i $PERF_DATA"
        echo_info "Generate flamegraph with: https://github.com/brendangregg/FlameGraph"
        ;;
        
    Darwin*)
        echo_info "Detected macOS - using Instruments (xctrace)"
        
        # Check if xctrace is available
        if ! command -v xcrun &> /dev/null; then
            echo_error "Xcode command line tools not found. Install with: xcode-select --install"
            exit 1
        fi
        
        TRACE_FILE="$OUTPUT_DIR/trace_${TIMESTAMP}.trace"
        
        echo_info "Recording with Time Profiler (filter: $FILTER)..."
        echo_warn "This may take a few seconds to initialize..."
        
        # Run xctrace with Time Profiler template
        xcrun xctrace record \
            --template "Time Profiler" \
            --output "$TRACE_FILE" \
            --launch -- "$BENCHMARK_BIN" \
                --benchmark_filter="$FILTER" \
                --benchmark_min_time=2
        
        echo ""
        echo_info "Trace saved to: $TRACE_FILE"
        echo_info "Open with: open $TRACE_FILE"
        echo ""
        echo_info "Or analyze from command line:"
        echo "  xcrun xctrace export --input $TRACE_FILE --output $OUTPUT_DIR/export_${TIMESTAMP}"
        
        # Automatically open in Instruments if running interactively
        if [ -t 1 ]; then
            read -p "Open trace in Instruments? [y/N] " -n 1 -r
            echo
            if [[ $REPLY =~ ^[Yy]$ ]]; then
                open "$TRACE_FILE"
            fi
        fi
        ;;
        
    *)
        echo_error "Unsupported OS: $(uname -s)"
        echo_info "Supported: Linux (perf), macOS (Instruments)"
        exit 1
        ;;
esac

# Also run benchmark with detailed stats
echo ""
echo_info "Running benchmark with counters..."
"$BENCHMARK_BIN" \
    --benchmark_filter="$FILTER" \
    --benchmark_counters_tabular=true \
    --benchmark_format=console \
    | tee "$OUTPUT_DIR/benchmark_${TIMESTAMP}.txt"

echo ""
echo_info "Profiling complete!"
echo_info "Results directory: $OUTPUT_DIR"
