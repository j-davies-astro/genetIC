#ifndef IC_TIPSY_HPP
#define IC_TIPSY_HPP

#include "src/io.hpp"
#include "src/simulation/particles/mapper/mapper.hpp"

namespace io {
  namespace tipsy {

	struct io_header_tipsy {
	  double scalefactor;
	  int n;
	  int ndim;
	  int ngas;
	  int ndark;
	  int nstar;
	} header_tipsy;


	template<typename GridType, typename FloatType=tools::datatypes::strip_complex<GridType>>
	void saveFieldTipsyArray(const std::string &filename,
							 particle::mapper::ParticleMapper<GridType> & mapper,
							 particle::AbstractMultiLevelParticleGenerator<GridType> & generator,
							 fields::MultiLevelField<GridType> &field) {
	  std::ofstream outfile(filename.c_str(), std::ofstream::binary);
	  int lengthField = mapper.size();
	  outfile.write(reinterpret_cast<char *>(&lengthField), 4);

	  field.toReal();

	  for (auto i = mapper.begin(generator); i != mapper.end(generator); ++i) {
		float data = float(tools::datatypes::real_part_if_complex(i.getField(field)));
		outfile.write(reinterpret_cast<char *>(&data), 4);
	  }
	}

	namespace TipsyParticle {

	  struct dark {
		float mass, x, y, z, vx, vy, vz, eps, phi;
	  };

	  struct gas {
		float mass, x, y, z, vx, vy, vz, rho, temp, eps, metals, phi;
	  };

	  template<typename T>
	  void initialise(dark &p, const cosmology::CosmologicalParameters<T>& /*&cosmo*/) {
		p.phi = 0.0;
	  }

	  template<typename T>
	  void initialise(gas &p, const cosmology::CosmologicalParameters<T> &cosmo) {
		p.temp = cosmo.TCMB / cosmo.scalefactor;
		p.metals = 0.0;
		p.rho = 0.0;
	  }
	}


	template<typename GridDataType, typename FloatType=tools::datatypes::strip_complex<GridDataType>>
	class TipsyOutput {
	protected:
        const particle::AbstractMultiLevelParticleGenerator<GridDataType> &generator;
      FILE *fd;
      std::ofstream photogenic_file;
      size_t iord;
      double pos_factor, vel_factor, mass_factor, min_mass, max_mass;
      double boxLength;
      std::shared_ptr<particle::mapper::ParticleMapper<GridDataType>> pMapper;
      const cosmology::CosmologicalParameters<FloatType> &cosmology;


	  template<typename ParticleType>
	  void saveTipsyParticlesFromBlock(std::vector<particle::Particle<FloatType>> &particleAr) {

		const size_t n = particleAr.size();

		/*
		for(auto &q: {xAr,yAr,zAr,vxAr,vyAr,vzAr,massAr,epsAr}) {
			assert(q.size()==n);
		}
		*/

		std::vector<ParticleType> p(n);

	#pragma omp parallel for
		for (size_t i = 0; i < n; i++) {
		  TipsyParticle::initialise(p[i], cosmology);
		  particle::Particle<FloatType> &thisParticle = particleAr[i];

		  p[i].x = thisParticle.pos.x * pos_factor - 0.5;
		  p[i].y = thisParticle.pos.y * pos_factor - 0.5;
		  p[i].z = thisParticle.pos.z * pos_factor - 0.5;
		  p[i].eps = thisParticle.soft * pos_factor;

		  p[i].vx = thisParticle.vel.x * vel_factor;
		  p[i].vy = thisParticle.vel.y * vel_factor;
		  p[i].vz = thisParticle.vel.z * vel_factor;
		  p[i].mass = thisParticle.mass * mass_factor;

#ifdef _OPENMP
		  if (thisParticle.mass == min_mass && omp_get_thread_num() == 0)
#else
		  if(thisParticle.mass==min_mass)
#endif
			photogenic_file << iord << std::endl;

		  ++iord;
		}

		fwrite(p.data(), sizeof(ParticleType), n, fd);
	  }

	  template<typename ParticleType>
	  void saveTipsyParticles(particle::mapper::MapperIterator<GridDataType> &&begin,
							  particle::mapper::MapperIterator<GridDataType> &&end) {

		ParticleType p;
		TipsyParticle::initialise(p, cosmology);
		std::vector<particle::Particle<FloatType>> particles;
		auto i = begin;
		while (i != end) {
		  i.getNextNParticles(particles);
		  saveTipsyParticlesFromBlock<ParticleType>(particles);
		}
	  }

	  template<typename ParticleType>
	  void initTipsyParticle(ParticleType &p) {

	  }


	  template<typename ParticleType>
	  void saveTipsyParticlesSingleThread(particle::mapper::MapperIterator<GridDataType> &&begin,
										  particle::mapper::MapperIterator<GridDataType> &&end) {

		ParticleType p;
		FloatType x, y, z, vx, vy, vz, mass, eps;

		for (auto i = begin; i != end; ++i) {
		  auto thisParticle = i.getParticle();

		  p.x = thisParticle.pos.x * pos_factor - 0.5;
		  p.y = thisParticle.pos.y * pos_factor - 0.5;
		  p.z = thisParticle.pos.z * pos_factor - 0.5;
		  p.eps = thisParticle.soft * pos_factor;

		  p.vx = thisParticle.vel.x * vel_factor;
		  p.vy = thisParticle.vel.y * vel_factor;
		  p.vz = thisParticle.vel.z * vel_factor;
		  p.mass = thisParticle.mass * mass_factor;

		  fwrite(&p, sizeof(ParticleType), 1, fd);

		  if (mass == min_mass) {
			photogenic_file << iord << std::endl;
		  }

		  ++iord;
		}
	  }


	public:

	  TipsyOutput(double boxLength,
				  const particle::AbstractMultiLevelParticleGenerator<GridDataType> &generator,
				  std::shared_ptr<particle::mapper::ParticleMapper<GridDataType>> pMapper,
				  const cosmology::CosmologicalParameters<FloatType> &cosmology) : generator(generator),boxLength(boxLength),
																	  pMapper(pMapper),
																	  cosmology(cosmology) {

	  }

	  void operator()(const std::string &filename) {

		// originally:
		// pmass in 1e10 h^-1 Msol
		// pos in Mpc h^-1
		// vel in km s^-1 a^1/2


		min_mass = std::numeric_limits<double>::max();
		max_mass = 0.0;

		FloatType mass, tot_mass = 0.0;

		for (auto i = pMapper->begin(generator); i != pMapper->end(generator); ++i) {
		  // progress("Pre-write scan file",iord, totlen);
		  mass = i.getMass(); // sometimes can be MUCH faster than getParticle
		  if (min_mass > mass) min_mass = mass;
		  if (max_mass < mass) max_mass = mass;
		  tot_mass += mass;
		}

		// end_progress();

		if (min_mass != max_mass) {
		  photogenic_file.open("photogenic.txt");
		}

		mass_factor = cosmology.OmegaM0 / tot_mass; // tipsy convention: sum(mass)=Om0
		pos_factor = 1. / boxLength;              // boxsize = 1

		double dKpcUnit = boxLength * 1000 / cosmology.hubble;
		double dMsolUnit = 1e10 / cosmology.hubble / mass_factor;
		double dKmsUnit = sqrt(4.3022682e-6 * dMsolUnit / (dKpcUnit));

		vel_factor = std::pow(cosmology.scalefactor, -0.5) / dKmsUnit;


		io_header_tipsy header;

		header.scalefactor = cosmology.scalefactor;
		header.n = pMapper->size();
		header.ndim = 3;
		header.ngas = pMapper->size_gas();
		header.ndark = pMapper->size_dm();
		header.nstar = 0;


		std::ofstream paramfile;
		paramfile.open("tipsy.param");

		paramfile << "dKpcUnit = " << dKpcUnit << std::endl;
		paramfile << "dMsolUnit = " << dMsolUnit << std::endl;
		paramfile << "dHubble0 = " << 0.1 * cosmology.hubble * dKpcUnit / dKmsUnit << std::endl;
		paramfile << "bComove = 1 " << std::endl;

		paramfile.close();

		fd = fopen(filename.c_str(), "w");
		if (!fd) throw std::runtime_error("Unable to open file for writing");


		fwrite(&header, sizeof(io_header_tipsy), 1, fd);

		saveTipsyParticles<TipsyParticle::gas>(pMapper->beginGas(generator), pMapper->endGas(generator));
		saveTipsyParticles<TipsyParticle::dark>(pMapper->beginDm(generator), pMapper->endDm(generator));

	  }
	};

	template<typename GridDataType, typename T>
	void save(const std::string &filename, double Boxlength,
			  const particle::AbstractMultiLevelParticleGenerator<GridDataType> & generator,
			  std::shared_ptr<particle::mapper::ParticleMapper<GridDataType>> pMapper,
			  const cosmology::CosmologicalParameters<T> &cosmology) {

	  TipsyOutput<GridDataType> output(Boxlength, generator, pMapper, cosmology);
	  output(filename);
	}



  }
}

#endif //IC_TIPSY_HPP