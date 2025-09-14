import numpy as np
import matplotlib.pyplot as plt
import math
# Constants
C = 1 * 1024**4 * 8 /2 # 512 GB in bits
b1 = 128  # 128 bits
b2 = 256  # 256 bits
conversion_factor_Y = 1 / (8 * 1024**3)  # Convert from bits to GB for Y
conversion_factor_P = 1 / (8 * 1024)  # Convert from bits to KB for P
# Constant M
M = 56  # You will specify this later
# Create a range of P values in bits (from 0 KB to 128 KB) and convert to KB
P_bits = np.linspace(1024*8, 64 * 1024 * 8, 400)  # Range in bits (0 to 128 KB)
P_KB = P_bits * conversion_factor_P
# Calculate q for both block sizes
q_b1 = P_bits / b1
q_b2 = P_bits / b2
# Using the modified formula with q as the exponent
log_base_e2 = np.log2(np.e)  # Cache log base change from e to 2
Y_b1 = (C / P_bits) * (np.floor(log_base_e2 + q_b1 * (np.log2(q_b1) - log_base_e2)) - M) * conversion_factor_Y
Y_b2 = (C / P_bits) * (np.floor(log_base_e2 + q_b2 * (np.log2(q_b2) - log_base_e2)) - M) * conversion_factor_Y
# Save P_KB, Y_b1, Y_b2 to CSV
data = np.column_stack((P_KB, Y_b1, Y_b2))
# Add the row [0, 0, 0] to the beginning of the data
data = np.vstack(([0, 0, 0], data))
np.savetxt("results/new_cap_data.csv", data, delimiter=",", header="P_KB,Y_b1,Y_b2", comments="")
# Confirming that the data has been saved
print("Data saved to 'new_cap_data.csv'.")
