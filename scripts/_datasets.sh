# ERWT3D dataset paths
# Sourced by other scripts. Sets RAW/ERWT/NX/NY/NZ/LABEL via resolve_dataset().

SMALL_RAW=/mnt/d/CUP/cup_3d_small.dat
SMALL_ERWT=/mnt/d/CUP/cup_3d_small.erwt3d
SMALL_NX=801; SMALL_NY=2405; SMALL_NZ=2501
SMALL_LABEL="20GB"

BIG_RAW=/mnt/d/CUP/cup_3d_big.dat
BIG_ERWT=/mnt/d/CUP/cup_3d_big.erwt3d
BIG_NX=2001; BIG_NY=2201; BIG_NZ=3000
BIG_LABEL="50GB"

# Resolve dataset name to env vars: RAW ERWT NX NY NZ LABEL
# Usage: resolve_dataset small|big
resolve_dataset() {
    case "$1" in
        small)
            RAW="$SMALL_RAW"; ERWT="$SMALL_ERWT"
            NX=$SMALL_NX; NY=$SMALL_NY; NZ=$SMALL_NZ
            LABEL="$SMALL_LABEL" ;;
        big)
            RAW="$BIG_RAW"; ERWT="$BIG_ERWT"
            NX=$BIG_NX; NY=$BIG_NY; NZ=$BIG_NZ
            LABEL="$BIG_LABEL" ;;
        *)
            echo "Error: unknown dataset '$1' (valid: small|big)" >&2
            return 1 ;;
    esac
}
