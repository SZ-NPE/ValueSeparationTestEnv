import csv
import matplotlib.pyplot as plt
import numpy as np

def main():

    arr_cpu_usage=[]

    with open('score.csv', "r", encoding='utf8', newline='') as f:
        reader = csv.reader(f)
        next(reader)
        for row in reader:
            arr_cpu_usage.append(row[1])




if __name__ == '__main__':
