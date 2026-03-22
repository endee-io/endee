"""
seed_sample_data.py — Seeds Endee with 25 academic chunks across 5 domains.
Run from the academiq/ directory:
  python scripts/seed_sample_data.py
"""

import sys
import os

# Allow running from academiq/ directory
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'backend'))

from app.rag_pipeline import RAGPipeline

SEED_DATA = [
    # ── Machine Learning (5 chunks) ──────────────────────────────────
    ("ml_transformers", "machine_learning",
     "Transformer architectures revolutionized natural language processing by introducing the self-attention mechanism. "
     "Unlike recurrent models, transformers process all tokens in parallel, enabling significant speedups during training. "
     "The attention mechanism allows the model to weigh the importance of each word relative to every other word in the sequence. "
     "This global context awareness is what enables transformers to capture long-range dependencies that RNNs often miss."),

    ("ml_backprop", "machine_learning",
     "Backpropagation computes gradients by applying the chain rule of calculus through each layer of a neural network. "
     "During the forward pass, activations are cached at every layer for use in the backward pass. "
     "The optimizer then uses these gradients to update weights in the direction that minimizes the loss function. "
     "Stochastic gradient descent and its variants like Adam and RMSProp are the most widely used optimization algorithms in deep learning."),

    ("ml_overfitting", "machine_learning",
     "Overfitting occurs when a model learns the noise and specific patterns of training data rather than generalizable features. "
     "Regularization techniques such as L1 and L2 penalties, dropout, and early stopping are standard countermeasures. "
     "Dropout randomly deactivates neurons during training, forcing the network to learn redundant representations. "
     "Cross-validation is the standard methodology for detecting and measuring overfitting objectively."),

    ("ml_cnn", "machine_learning",
     "Convolutional neural networks exploit spatial locality by applying learnable filter kernels across input feature maps. "
     "Each convolutional layer extracts increasingly abstract representations: early layers detect edges and textures, "
     "while deeper layers recognize semantic concepts. Pooling layers progressively reduce spatial dimensions to achieve "
     "translation invariance. CNNs have achieved superhuman accuracy on ImageNet classification benchmarks since AlexNet in 2012."),

    ("ml_rl", "machine_learning",
     "Reinforcement learning frames decision-making as an agent interacting with an environment to maximize cumulative reward. "
     "The agent learns a policy that maps states to actions through trial and error, guided by reward signals. "
     "Deep Q-Networks combine Q-learning with neural function approximation, enabling RL to scale to high-dimensional state spaces like raw pixels. "
     "AlphaGo demonstrated that RL combined with Monte Carlo tree search can surpass human-level performance in complex board games."),

    # ── Quantum Computing (5 chunks) ────────────────────────────────
    ("qc_qubits", "quantum_computing",
     "Quantum bits, or qubits, are the fundamental units of quantum information. "
     "Unlike classical bits that are deterministically either 0 or 1, qubits exist in a superposition of both states simultaneously until measured. "
     "Physical implementations include superconducting circuits, trapped ions, photonic systems, and topological qubits. "
     "Maintaining qubit coherence long enough to perform computations remains the central engineering challenge in the field."),

    ("qc_entanglement", "quantum_computing",
     "Quantum entanglement is a non-classical correlation where the state of one qubit instantaneously determines the state of its entangled partner, "
     "regardless of physical separation. Einstein famously called this phenomenon 'spooky action at a distance'. "
     "Entanglement is a computational resource that enables quantum algorithms to explore exponentially many states simultaneously. "
     "Bell's theorem mathematically proves that entanglement cannot be explained by any local hidden variable theory."),

    ("qc_algorithms", "quantum_computing",
     "Shor's algorithm achieves exponential speedup over classical methods for integer factorization, with profound implications for RSA cryptography. "
     "Grover's algorithm provides a quadratic speedup for unstructured database search. "
     "Quantum Phase Estimation is a subroutine used in many quantum algorithms, including those for simulating quantum chemistry. "
     "These algorithms are theoretically proven to outperform all classical counterparts on specific problem classes."),

    ("qc_error", "quantum_computing",
     "Quantum error correction is necessary because qubits are extraordinarily sensitive to environmental noise, a problem called decoherence. "
     "The surface code is currently the leading error correction scheme, requiring approximately 1000 physical qubits to produce one "
     "logical qubit with fault-tolerant properties. Achieving fault tolerance requires error rates below the threshold of roughly 1 percent "
     "per gate operation. Most current quantum processors are in the NISQ era, meaning noisy intermediate-scale quantum devices."),

    ("qc_applications", "quantum_computing",
     "Near-term quantum advantage is expected first in quantum chemistry simulation, where classical computers struggle with exponential state-space growth. "
     "Drug discovery and materials science could be transformed by accurate simulation of molecular electronic structure. "
     "Quantum machine learning proposes to accelerate kernel methods and gradient computations. "
     "Financial portfolio optimization and logistics represent promising applications for quantum annealing approaches."),

    # ── Climate Science (5 chunks) ──────────────────────────────────
    ("cs_carbon", "climate_science",
     "The global carbon cycle describes the continuous exchange of carbon among the atmosphere, oceans, terrestrial biosphere, and lithosphere. "
     "Photosynthesis removes approximately 120 gigatons of carbon per year from the atmosphere, while respiration and decomposition return comparable amounts. "
     "Human activities, primarily fossil fuel combustion and deforestation, have introduced an additional 10 gigatons annually since industrialization. "
     "Ocean uptake currently absorbs roughly 25 percent of anthropogenic CO2 emissions."),

    ("cs_feedback", "climate_science",
     "Climate feedback mechanisms amplify or dampen the initial warming caused by greenhouse gas forcing. "
     "The ice-albedo feedback is a powerful positive loop: melting Arctic sea ice exposes dark ocean water, which absorbs more solar radiation, "
     "causing further warming and more melting. Water vapor is the strongest amplifying feedback, as warmer air holds more moisture, "
     "intensifying the greenhouse effect. Cloud feedbacks remain the largest source of uncertainty in climate model projections."),

    ("cs_tipping", "climate_science",
     "Tipping points are thresholds in the climate system beyond which self-sustaining change becomes inevitable even if greenhouse gas concentrations stabilize. "
     "The collapse of the West Antarctic Ice Sheet, dieback of the Amazon rainforest, and thaw of permafrost are among the most consequential potential tipping points. "
     "Some researchers argue that crossing one tipping point can trigger cascading effects across others. "
     "The 1.5 degree Celsius target in the Paris Agreement was specifically designed to reduce the risk of activating these tipping elements."),

    ("cs_mitigation", "climate_science",
     "Mitigation strategies aim to reduce greenhouse gas emissions at their source through energy system transformation. "
     "Renewable energy, particularly solar photovoltaics and wind power, have experienced cost reductions exceeding 90 percent in the past decade. "
     "Carbon capture and storage involves sequestering CO2 from point sources or directly from the atmosphere. "
     "The IPCC Sixth Assessment Report concludes that limiting warming to 1.5 degrees requires reaching net-zero CO2 emissions by approximately 2050 globally."),

    ("cs_ocean", "climate_science",
     "The ocean plays a critical role in regulating Earth's climate by absorbing over 90 percent of excess heat trapped by greenhouse gases since 1970. "
     "Ocean heat content has reached record levels each of the last several years, driving intensification of tropical cyclones and sea level rise through thermal expansion. "
     "Ocean acidification, caused by absorption of anthropogenic CO2, threatens marine ecosystems particularly coral reefs and shell-forming organisms. "
     "Thermohaline circulation, sometimes called the ocean conveyor belt, redistributes heat across the planet and may be weakening due to freshwater influx from melting ice."),

    # ── Neuroscience (5 chunks) ─────────────────────────────────────
    ("ns_plasticity", "neuroscience",
     "Synaptic plasticity refers to the activity-dependent modification of synaptic strength and is widely considered the cellular basis of learning and memory. "
     "Long-term potentiation is a form of plasticity where repeated activation of a synapse leads to persistent strengthening of that connection. "
     "Conversely, long-term depression weakens synaptic connections through low-frequency stimulation. "
     "The principle that neurons that fire together wire together, known as Hebb's postulate, elegantly captures the associative nature of synaptic plasticity."),

    ("ns_memory", "neuroscience",
     "Memory consolidation is the process by which newly acquired information is stabilized from a fragile short-term state into a durable long-term form. "
     "The hippocampus plays an indispensable role in encoding episodic and semantic memories, as demonstrated by the famous patient H.M. "
     "who became profoundly amnesic following hippocampal removal. Sleep, particularly slow-wave sleep, is now understood to be critical for memory "
     "consolidation through replay of daytime experiences. The standard model of systems consolidation proposes that memories gradually transfer "
     "from hippocampus to neocortex over weeks to years."),

    ("ns_neurotransmitters", "neuroscience",
     "Neurotransmitters are chemical messengers that propagate signals across the synaptic cleft between neurons. "
     "Glutamate is the primary excitatory neurotransmitter in the central nervous system, while GABA serves as the principal inhibitory signal. "
     "Dopamine plays a key role in reward prediction and motivation, and its dysregulation is implicated in addiction, Parkinson's disease, and schizophrenia. "
     "Serotonin modulates mood, appetite, and sleep, and is the primary target of SSRI antidepressant medications."),

    ("ns_neurogenesis", "neuroscience",
     "Adult neurogenesis, the generation of new neurons in the mature brain, was long considered impossible but is now accepted to occur in specific brain regions. "
     "The hippocampal dentate gyrus and the olfactory bulb are the two primary sites of ongoing neurogenesis in adult mammals. "
     "Physical exercise is one of the most potent known stimulators of adult hippocampal neurogenesis, which may partly explain its cognitive benefits. "
     "The functional role of adult-born neurons is thought to involve pattern separation and the encoding of temporally distinct memories."),

    ("ns_connectome", "neuroscience",
     "The connectome is a comprehensive map of all synaptic connections within a nervous system. "
     "The Human Connectome Project aims to map the structural and functional connectivity of the entire human brain using high-resolution diffusion MRI and fMRI. "
     "The only complete connectome currently available is that of C. elegans, a nematode with exactly 302 neurons and approximately 7000 synapses. "
     "Understanding the connectome may reveal how brain structure constrains cognitive function and how neurological disorders disrupt normal connectivity patterns."),

    # ── Bioinformatics (5 chunks) ───────────────────────────────────
    ("bi_sequencing", "bioinformatics",
     "Next-generation sequencing technologies have reduced the cost of sequencing a human genome from three billion dollars to under one thousand dollars within two decades. "
     "Short-read platforms like Illumina generate billions of 150 base-pair reads per run with very low error rates. "
     "Long-read technologies from Oxford Nanopore and PacBio can span repetitive regions and structural variants that short reads miss. "
     "Whole-genome sequencing, whole-exome sequencing, and RNA-seq are now routine tools in clinical genomics and biomedical research."),

    ("bi_proteinfold", "bioinformatics",
     "Protein structure prediction was transformed by AlphaFold2, developed by DeepMind, which achieved atomic-level accuracy competitive with experimental methods. "
     "The three-dimensional structure of a protein is determined by its primary amino acid sequence through a complex, energy-driven folding process. "
     "Understanding protein structure is fundamental to drug discovery because binding pockets and active sites are three-dimensional features. "
     "The AlphaFold Protein Structure Database now contains predicted structures for over 200 million proteins, covering virtually every known protein in the UniProt database."),

    ("bi_crispr", "bioinformatics",
     "CRISPR-Cas9 is a genome editing technology derived from a bacterial adaptive immune system that allows precise modification of DNA sequences in virtually any organism. "
     "The Cas9 protein acts as molecular scissors guided to a specific genomic location by a single guide RNA complementary to the target sequence. "
     "CRISPR has applications in correcting genetic diseases, developing cancer therapies, creating disease-resistant crops, and producing model organisms for research. "
     "Base editing and prime editing are newer CRISPR variants that can introduce precise single-nucleotide changes without creating double-strand breaks."),

    ("bi_metagenomics", "bioinformatics",
     "Metagenomics is the study of genetic material recovered directly from environmental samples, enabling characterization of microbial communities without requiring laboratory cultivation. "
     "The human gut microbiome contains approximately 38 trillion microbial cells encoding a collective gene set 150 times larger than the human genome. "
     "Shotgun metagenomics sequences all DNA in a sample, enabling identification of novel organisms and functional gene pathways. "
     "Dysbiosis of the gut microbiome has been associated with inflammatory bowel disease, obesity, type 2 diabetes, and even neuropsychiatric conditions through the gut-brain axis."),

    ("bi_gwas", "bioinformatics",
     "Genome-wide association studies scan hundreds of thousands of genetic variants across many individuals to identify loci statistically associated with complex traits or diseases. "
     "GWAS have identified thousands of genetic variants associated with conditions including type 2 diabetes, schizophrenia, and coronary artery disease. "
     "The majority of GWAS hits fall in non-coding regulatory regions of the genome, highlighting the importance of gene expression regulation in disease. "
     "Polygenic risk scores aggregate the effects of thousands of variants to produce personalized genetic risk predictions that are increasingly being evaluated in clinical settings."),
]


def main():
    pipeline = RAGPipeline()
    total = len(SEED_DATA)
    print(f"Seeding {total} academic chunks into Endee...\n")

    success_count = 0
    for i, (doc_id, domain, text) in enumerate(SEED_DATA, 1):
        result = pipeline.ingest_text(text, doc_name=f"{domain}/{doc_id}")
        status = "✓" if result["status"] == "success" else "✗"
        if result["status"] == "success":
            success_count += 1
        print(f"[{i:2}/{total}] {status} {domain}/{doc_id} → {result.get('chunks_indexed', 0)} chunk(s)")

    print(f"\n✅ Seeding complete! {success_count}/{total} documents indexed into Endee.")
    print("\nNow try these queries:")
    print('  POST /api/search  {"query": "how does attention work in transformers"}')
    print('  POST /api/query   {"question": "How does backpropagation work?"}')
    print('  POST /api/search  {"query": "quantum entanglement", "doc_name_filter": "quantum_computing/qc_entanglement"}')


if __name__ == "__main__":
    main()
