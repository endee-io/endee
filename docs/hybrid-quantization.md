# Hybrid Quantization

We utilize a hybrid quantization approach to optimize both performance and memory usage in our HNSW index structure.

## Strategy

For the upper layers of the HNSW graph, we employ **int8 quantization**. 

During a hierarchical search:
1.  **Upper Layers**: Searches are greedy and primarily serve to navigate towards the general neighborhood of the target vector. High precision is less critical here.
2.  **Base Layer**: Most distance calculations and the final candidate selection happen at the base layer (layer 0), where we can maintain higher precision or different storage strategies.

## Benefits

*   **Reduced Space Requirements**: Storing upper layer vectors in int8 format significantly lowers the memory footprint compared to float32.
*   **Performance**: Since the upper layers are traversed quickly to find an entry point for the base layer, and the majority of the computational work (exhaustive search) happens at the bottom, this trade-off yields lower memory usage with no impact on search accuracy or latency.

## Note

* It can be disabled with -D DISABLE_HYBRID_QUANTIZATION compile time flag