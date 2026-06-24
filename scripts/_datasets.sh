# ERWT3D dataset paths
# Sourced by other scripts. Sets RAW/ERWT/DIM/LABEL env vars via resolve_dataset().

SMALL_RAW=/mnt/d/CUP/cup_3d_small.dat
SMALL_ERWT=/mnt/d/CUP/cup_3d_small.erwt3d
SMALL_DIM="801 2405 2501"
SMALL_LABEL="20GB"

BIG_RAW=/mnt/d/CUP/cup_3d_big.dat
BIG_ERWT=/mnt/d/CUP/cup_3d_big.erwt3d
BIG_DIM="2001 2201 3000"
BIG_LABEL="50GB"

# Resolve dataset name to env vars: RAW ERWT DIM LABEL
# Usage: resolve_dataset small|big
resolve_dataset() {
    case "$1" in
        small)
            RAW="$SMALL_RAW"; ERWT="$SMALL_ERWT"
            DIM="$SMALL_DIM"; LABEL="$SMALL_LABEL" ;;
        big)
            RAW="$BIG_RAW"; ERWT="$BIG_ERWT"
            DIM="$BIG_DIM"; LABEL="$BIG_LABEL" ;;
        *)
            echo "Error: unknown dataset '$1' (valid: small|big)" >&2
            return 1 ;;
    esac
}